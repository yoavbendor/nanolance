#!/usr/bin/env python3
"""Lance datasets people have published on the Hugging Face Hub, read by nanolance and checked
against pylance.

    python tools/real_lance_check.py list [--search clip] [--max-gb 5]   # Lance datasets on the Hub
    python tools/real_lance_check.py suggested                            # the set below, and why
    python tools/real_lance_check.py fetch ID [ID ...] --dest DIR [--max-gb 1] [--only data/x.lance]
    python tools/real_lance_check.py check DIR [--rows 5000] [--takes 256] [--full] [--json out.json]
    python tools/real_lance_check.py run --dest DIR [--max-gb 1]          # fetch the suggested set, check
    python tools/real_lance_check.py rewrite DIR --dest OUT --version 2.2 # pylance rewrites them, then check OUT

Nothing is downloaded into this repository, and nothing is redistributed: each dataset keeps the
license its card on the Hub gives it.

fetch downloads a dataset's manifests (_versions/, _deletions/) and then its data files, smallest
first, while they fit in --max-gb per table (default 1). A table whose data files all came down is
checked as a dataset; a partial one is checked file by file, which is how a 4 TB dataset gets tested
from one of its files. Data files on the Hub are large -- fineweb-edu's smallest is 0.9 GB, laion-1m's
8 GB, and some tables are a single 48 GB file -- so a table whose smallest file does not fit is
skipped, and fetch says how much it needs. Index files (_indices/) are skipped unless --indices is given:
nanolance does not read indexes. A file that is already there at the right size is not fetched
again, so fetch resumes. Set HF_TOKEN for gated datasets; HF_ENDPOINT points at a mirror.

check finds every Lance table (a directory holding _versions/) and every data file under DIR and
reads each with both engines:

  dataset   schema, count_rows, the first --rows rows (or every fragment with --full), --takes
            random rows by take(), and take_blobs() of a few rows of each blob column
  file      each data file's first --rows rows and --takes random rows, by LanceFileReader
            (--files per table, default 3)

Outcomes, per check and per table as the worst of its checks:

  pass      equal to pylance
  type      equal values, another Arrow type
  refused   nanolance raised an error (it names what it does not read)
  mismatch  different rows from pylance. A wrong result: please report it with the dataset name
  pylance   pylance itself failed (a partial download, or a format newer than pylance 12.0.0)

rewrite has pylance write each table under DIR again (its first --rows rows; a partial download's
first data file) in another Lance file format -- --version 2.0, 2.1 or 2.2 -- cut into files of
--rows-per-file rows, with the same schema and field metadata. The Hub holds 2.0 and 2.1 tables
only; this is how real data gets tested in 2.2. Check OUT afterwards.

Each check also records both engines' wall time: the faster of --repeat runs (default 2), files in
the page cache.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import random
import struct
import sys
import time
import traceback
import urllib.parse
import urllib.request
from pathlib import Path

ENDPOINT = os.environ.get("HF_ENDPOINT", "https://huggingface.co").rstrip("/")
MARKER = ".nanolance_fetch.json"

# Picked to cover what people store in Lance: embeddings of several widths, images, audio, video,
# long text, blob columns, LanceDB tables, and writers from pylance releases a year apart. Sizes are
# the whole dataset; fetch --max-gb takes the first files of the larger ones.
SUGGESTED = [
    # (id, only these tables or None, what is in it)
    ("lancedb/magical_kingdom", None, "LanceDB tables, a few MB: text and vectors"),
    ("lhoestq/wiki-dpr-lance-example", None, "Wikipedia passages with 768-d DPR embeddings (40 MB)"),
    ("prrao87/tea-hypervectors", None, "high-dimensional hypervectors and images (60 MB)"),
    ("Jacob235/fred-vector-index", None, "FRED series text and embeddings, LanceDB (120 MB)"),
    ("Litian2002/robotics-papers-vecdb", None, "paper chunks and embeddings, LanceDB (460 MB)"),
    ("davanstrien/emb-test-wiki-lance", None, "Wikipedia with embeddings (490 MB)"),
    ("lance-format/mnist-lance", None, "images (PNG bytes), labels (170 MB)"),
    ("lance-format/eurosat-lance", None, "satellite images (110 MB)"),
    ("lance-format/handwriting-ocr", None, "images and transcriptions (40 MB)"),
    ("lance-format/squad-v2-lance", None, "question answering text (290 MB)"),
    ("carpelan/sbl-lance", None, "long Swedish text, written by another team (100 MB)"),
    ("lance-format/librispeech-clean-lance", ["data/dev_clean.lance"], "audio clips and transcripts (340 MB)"),
    ("lance-format/ms-marco-v2.1-lance", ["data/validation.lance"], "passages and queries (410 MB)"),
    ("lance-format/coco-captions-2017-lance", ["data/val.lance"], "COCO images and captions (530 MB)"),
    # One data file of these is more than the default --max-gb 1; each says what it needs.
    ("lance-format/fineweb-edu", None, "web text with 384-d embeddings; 4 TB, files of 0.9-6 GB"),
    ("lance-format/laion-1m", None, "JPEGs with CLIP embeddings; files of 8-74 GB"),
    ("lance-format/openvid-lance", None, "MP4 blobs with 1024-d embeddings; files of 10-19 GB"),
    ("jrmiller/coco-2017-siglip2-embeddings", None, "COCO images with SigLIP2 embeddings; one 48 GB file"),
]


# ── Hub ──────────────────────────────────────────────────────────────────────────────────────────

def _open(url: str, timeout: int = 120):
    req = urllib.request.Request(url, headers={"User-Agent": "nanolance-real-lance-check"})
    token = os.environ.get("HF_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    return urllib.request.urlopen(req, timeout=timeout)


def _get_json(url: str):
    with _open(url) as r:
        link = r.headers.get("Link", "")
        return json.load(r), link


def hub_tree(repo: str, revision: str = "main") -> list:
    """Every file in a dataset repo, following the API's pages."""
    url = f"{ENDPOINT}/api/datasets/{repo}/tree/{revision}?recursive=true"
    files = []
    while url:
        page, link = _get_json(url)
        files += [f for f in page if f.get("type") == "file"]
        url = None
        for part in link.split(","):
            if 'rel="next"' in part:
                url = part[part.index("<") + 1:part.index(">")]
    return files


def hub_list(search: str | None) -> list:
    q = "filter=format:lance&limit=1000&expand[]=mainSize&expand[]=downloads&expand[]=tags"
    if search:
        q += "&search=" + urllib.parse.quote(search)
    rows, _ = _get_json(f"{ENDPOINT}/api/datasets?{q}")
    return rows


def _split_tables(files: list) -> tuple:
    """Files grouped by the Lance table they belong to (a directory holding _versions/), and the rest."""
    roots = {f["path"].split("/_versions/")[0] if "/_versions/" in f["path"] else ""
             for f in files if f["path"].startswith("_versions/") or "/_versions/" in f["path"]}
    tables: dict = {r: [] for r in roots}
    other = []
    for f in files:
        owner = max((r for r in roots if r == "" or f["path"].startswith(r + "/")), key=len, default=None)
        (tables[owner] if owner is not None else other).append(f)
    return tables, other


def _rel(root: str, path: str) -> str:
    return path[len(root) + 1:] if root else path


def fetch(repo: str, dest: Path, max_gb: float, only: list | None, indices: bool, jobs: int) -> dict:
    files = hub_tree(repo)
    tables, other = _split_tables(files)
    if only:
        tables = {r: fs for r, fs in tables.items() if any(r == o.rstrip("/") or r.startswith(o.rstrip("/") + "/")
                                                            for o in only)}
    budget = int(max_gb * 1e9)
    want, report = [], {"repo": repo, "tables": {}}
    want += [f for f in other if f["path"].lower().startswith("readme") and f.get("size", 0) < 5_000_000]
    for root, fs in sorted(tables.items()):
        meta = [f for f in fs if _rel(root, f["path"]).split("/")[0] in ("_versions", "_deletions")]
        if indices:
            meta += [f for f in fs if _rel(root, f["path"]).startswith("_indices/")]
        data = [f for f in fs if _rel(root, f["path"]).startswith("data/")]
        lance_files = [f for f in data if f["path"].endswith(".lance")]
        # The smallest data files that fit: on the Hub one is often several GB.
        chosen, used = [], sum(f.get("size", 0) for f in meta)
        for f in sorted(lance_files, key=lambda f: (f.get("size", 0), f["path"])):
            if used + f.get("size", 0) > budget:
                break
            chosen.append(f)
            used += f.get("size", 0)
        if lance_files and not chosen:
            smallest = min(f.get("size", 0) for f in lance_files)
            print(f"  {root or '.'}: skipped, its smallest data file is {smallest / 1e9:.2f} GB "
                  f"(--max-gb {max_gb:g})")
            report["tables"][root or "."] = {"data_files": len(lance_files), "fetched_files": 0, "complete": False,
                                             "skipped": True, "smallest_data_file": smallest}
            continue
        # A data file's blob sidecars live in data/<file stem>/.
        stems = {f["path"][:-len(".lance")] + "/" for f in chosen}
        blobs = [f for f in data if f["path"].endswith(".blob") and any(f["path"].startswith(s) for s in stems)]
        want += meta + chosen + blobs
        report["tables"][root or "."] = {
            "data_files": len(lance_files), "fetched_files": len(chosen),
            "complete": len(chosen) == len(lance_files),
            "bytes": sum(f.get("size", 0) for f in chosen + blobs + meta),
            "dataset_bytes": sum(f.get("size", 0) for f in fs),
        }
    base = dest / repo.replace("/", "__")
    total = sum(f.get("size", 0) for f in want)
    print(f"{repo}: {len(tables)} table(s), fetching {len(want)} files, {total / 1e9:.2f} GB", flush=True)

    def one(f):
        out = base / f["path"]
        size = f.get("size")
        if out.exists() and (size is None or out.stat().st_size == size):
            return 0
        out.parent.mkdir(parents=True, exist_ok=True)
        part = out.with_name(out.name + ".part")
        url = f"{ENDPOINT}/datasets/{repo}/resolve/main/{urllib.parse.quote(f['path'])}"
        for attempt in range(4):
            try:
                with _open(url, timeout=300) as r, open(part, "wb") as w:
                    while chunk := r.read(1 << 22):
                        w.write(chunk)
                part.replace(out)
                return size or 0
            except Exception:  # noqa: BLE001 -- retried, then raised
                if attempt == 3:
                    raise
                time.sleep(2 ** (attempt + 1))
        return 0

    done = 0
    with cf.ThreadPoolExecutor(jobs) as ex:
        for n in ex.map(one, want):
            done += n
    base.mkdir(parents=True, exist_ok=True)
    (base / MARKER).write_text(json.dumps(report, indent=2))
    for root, t in report["tables"].items():
        if t.get("skipped"):
            continue
        state = "complete" if t["complete"] else f"{t['fetched_files']} of {t['data_files']} data files"
        print(f"  {root}: {state}")
    return report


# ── Checking ─────────────────────────────────────────────────────────────────────────────────────

RANK = {"pass": 0, "type": 1, "pylance": 2, "refused": 3, "mismatch": 4}


def _first_difference(got, want) -> int:
    """The first row where two equal-length chunked arrays differ, by halving."""
    lo, n = 0, len(want)
    while n > 1:
        half = n // 2
        if not got.slice(lo, half).equals(want.slice(lo, half)):
            n = half
        else:
            lo, n = lo + half, n - half
    return lo


def _short(v, limit: int = 160) -> str:
    s = repr(v)
    return s if len(s) <= limit else s[:limit] + "..."


def compare(got, want) -> tuple:
    import pyarrow as pa
    if got.num_rows != want.num_rows:
        return "mismatch", f"{got.num_rows} rows, pylance {want.num_rows}"
    if got.schema.names != want.schema.names:
        return "mismatch", f"columns {got.schema.names}, pylance {want.schema.names}"
    outcome, notes = "pass", []
    for name in want.schema.names:
        g, w = got.column(name), want.column(name)
        if g.type != w.type:
            try:
                same = g.cast(w.type).equals(w)
            except (pa.ArrowInvalid, pa.ArrowNotImplementedError):
                same = False
            if not same:
                return "mismatch", f"{name}: {g.type}, pylance {w.type}"
            outcome = "type"
            notes.append(f"{name}: {g.type}, pylance {w.type}")
            continue
        if not g.equals(w):
            row = _first_difference(g, w)
            if g.slice(row, 1).to_pylist() == w.slice(row, 1).to_pylist():  # NaN, -0.0 and the like
                continue
            return "mismatch", (f"{name} row {row}: {_short(g.slice(row, 1).to_pylist()[0])}, "
                                f"pylance {_short(w.slice(row, 1).to_pylist()[0])}")
    return outcome, "; ".join(notes)


REPEAT = 2


def _timed(fn):
    """The result and the faster of REPEAT runs: the first also pays for the page cache and warm-up."""
    best, out = float("inf"), None
    for _ in range(REPEAT):
        t = time.perf_counter()
        out = fn()
        best = min(best, time.perf_counter() - t)
    return out, best


def _check(results: list, where: str, what: str, ours, theirs):
    """Runs pylance, then nanolance, and records the outcome."""
    rec = {"where": where, "what": what}
    try:
        want, rec["pylance_s"] = _timed(theirs)
    except Exception as e:  # noqa: BLE001 -- pylance's own failure is an outcome
        rec.update(outcome="pylance", detail=f"{type(e).__name__}: {str(e).splitlines()[0][:300]}")
        results.append(rec)
        return None
    try:
        got, rec["nanolance_s"] = _timed(ours)
    except Exception as e:  # noqa: BLE001 -- nanolance's refusal is an outcome
        rec.update(outcome="refused", detail=f"{type(e).__name__}: {str(e).splitlines()[0][:300]}")
        results.append(rec)
        return want
    if callable(getattr(want, "equals", None)) and hasattr(want, "schema") and hasattr(want, "num_rows"):
        rec["outcome"], rec["detail"] = compare(got, want)
        rec["rows"] = want.num_rows
    else:
        rec["outcome"] = "pass" if got == want else "mismatch"
        rec["detail"] = "" if got == want else f"{_short(got)}, pylance {_short(want)}"
    results.append(rec)
    return want


def _footer_version(path: Path) -> str:
    with open(path, "rb") as f:
        f.seek(-8, os.SEEK_END)
        major, minor, magic = struct.unpack("<HH4s", f.read(8))
    return f"{major}.{minor}" if magic == b"LANC" else "?"


def _is_blob(field) -> bool:
    md = field.metadata or {}
    if md.get(b"lance-encoding:blob") == b"true":
        return True
    return field.type.__class__.__name__ == "StructType" and md.get(b"lance-encoding:packed") is not None \
        and {f.name for f in field.type} >= {"kind", "position", "size"}


def check_table(root: Path, args, results: list, complete: bool):
    import lance
    import nanolance.lance as nl
    where = str(root.relative_to(args.dir)) if root != args.dir else "."
    rng = random.Random(0)
    if complete:
        _check(results, where, "open", lambda: nl.dataset(str(root)) is not None,
               lambda: lance.dataset(str(root)) is not None)
        try:
            theirs, ours = lance.dataset(str(root)), nl.dataset(str(root))
        except Exception:  # noqa: BLE001 -- recorded by "open" above
            theirs = ours = None
        if theirs is not None and ours is not None:
            _check(results, where, "schema", lambda: str(ours.schema.remove_metadata()),
                   lambda: str(theirs.schema.remove_metadata()))
            n = theirs.count_rows()
            _check(results, where, "count_rows", ours.count_rows, theirs.count_rows)
            if args.full:
                for frag in theirs.get_fragments():
                    fid = frag.fragment_id
                    _check(results, where, f"fragment {fid}", lambda fid=fid: ours.get_fragment(fid).to_table(),
                           lambda frag=frag: frag.to_table())
            else:
                _check(results, where, f"first {min(n, args.rows)} rows",
                       lambda: ours.to_table(limit=args.rows), lambda: theirs.to_table(limit=args.rows))
            idx = sorted(rng.sample(range(n), min(n, args.takes))) if n else []
            if idx:
                _check(results, where, f"take {len(idx)} random rows", lambda: ours.take(idx),
                       lambda: theirs.take(idx))
            for field in theirs.schema:
                if _is_blob(field) and idx:
                    some = idx[:5]
                    _check(results, where, f"take_blobs {field.name}",
                           lambda f=field.name: [b.readall() if b else None for b in ours.take_blobs(f, indices=some)],
                           lambda f=field.name: [b.readall() if b else None for b in theirs.take_blobs(f, indices=some)])
    data = sorted((root / "data").glob("*.lance")) if (root / "data").is_dir() else []
    for path in data[: args.files]:
        check_file(path, args, results, where)


def check_file(path: Path, args, results: list, where: str):
    import lance.file as lf
    import nanolance.lance.file as nf
    rng = random.Random(1)
    name = f"{where}/data/{path.name}" if where else path.name
    version = _footer_version(path)
    try:
        n = lf.LanceFileReader(str(path)).num_rows()
    except Exception as e:  # noqa: BLE001
        results.append({"where": name, "what": f"file ({version})", "outcome": "pylance",
                        "detail": f"{type(e).__name__}: {str(e).splitlines()[0][:300]}"})
        return
    k = min(n, args.rows)
    _check(results, name, f"file {version}: first {k} rows",
           lambda: nf.LanceFileReader(str(path)).read_range(0, k).to_table(),
           lambda: lf.LanceFileReader(str(path)).read_range(0, k).to_table())
    idx = sorted(rng.sample(range(n), min(n, args.takes))) if n else []
    if idx:
        _check(results, name, f"file {version}: take {len(idx)} random rows",
               lambda: nf.LanceFileReader(str(path)).take_rows(idx).to_table(),
               lambda: lf.LanceFileReader(str(path)).take_rows(idx).to_table())


def find_tables(base: Path) -> list:
    roots = sorted({p.parent for p in base.rglob("_versions") if p.is_dir()})
    return roots


def complete_tables(base: Path) -> dict:
    """What fetch recorded about each table: complete or partial."""
    out = {}
    for marker in base.rglob(MARKER):
        rep = json.loads(marker.read_text())
        for root, t in rep.get("tables", {}).items():
            out[(marker.parent / root).resolve()] = t["complete"]
    return out


def rewrite(args) -> int:
    """pylance writes each table under args.dir again in format args.version; the count written."""
    import lance
    import lance.file as lf
    import pyarrow as pa
    base = Path(args.dir).resolve()
    known = complete_tables(base)
    written = 0
    for root in find_tables(base):
        rel = root.relative_to(base) if root != base else Path("table")
        out = (args.dest / rel).resolve()
        try:
            if known.get(root.resolve(), True):
                table = lance.dataset(str(root)).to_table(limit=args.rows)
            else:
                data = sorted((root / "data").glob("*.lance"))
                if not data:
                    continue
                reader = lf.LanceFileReader(str(data[0]))
                table = reader.read_range(0, min(reader.num_rows(), args.rows)).to_table()
            if out.exists():
                import shutil
                shutil.rmtree(out)
            out.parent.mkdir(parents=True, exist_ok=True)
            lance.write_dataset(table, str(out), data_storage_version=args.version,
                                max_rows_per_file=max(1, args.rows_per_file))
            written += 1
            print(f"wrote    {rel}  {table.num_rows} rows, format {args.version}", flush=True)
        except Exception as e:  # noqa: BLE001 -- one table failing does not stop the rest
            print(f"skipped  {rel}: {type(e).__name__}: {str(e).splitlines()[0][:200] if str(e) else ''}",
                  flush=True)
    return written


def check(args) -> list:
    args.dir = Path(args.dir).resolve()
    known = complete_tables(args.dir)
    results = []
    for root in find_tables(args.dir):
        complete = known.get(root.resolve(), True)
        print(f"── {root.relative_to(args.dir) if root != args.dir else '.'}"
              f"{'' if complete else '  (partial download: checking its files)'}", flush=True)
        start = len(results)
        try:
            check_table(root, args, results, complete)
        except Exception as e:  # noqa: BLE001 -- keep going through the other tables
            results.append({"where": str(root), "what": "check", "outcome": "refused",
                            "detail": f"{type(e).__name__}: {e}"})
            if args.verbose:
                traceback.print_exc()
        for r in results[start:]:
            r["table"] = str(root.relative_to(args.dir)) if root != args.dir else "."
            t = ""
            if "nanolance_s" in r:
                t = f"  nanolance {r['nanolance_s'] * 1e3:8.1f} ms  pylance {r['pylance_s'] * 1e3:8.1f} ms"
            print(f"   {r['outcome']:8} {r['what']:40}{t}  {r.get('detail', '')}".rstrip(), flush=True)
    return results


def summarize(results: list):
    by_where: dict = {}
    for r in results:
        by_where.setdefault(r.get("table", r["where"]), []).append(r)
    counts: dict = {}
    print("\nPer table (worst outcome of its checks):")
    for where, rs in by_where.items():
        worst = max(rs, key=lambda r: RANK.get(r["outcome"], 9))
        counts[worst["outcome"]] = counts.get(worst["outcome"], 0) + 1
        ours = sum(r.get("nanolance_s", 0) for r in rs)
        theirs = sum(r.get("pylance_s", 0) for r in rs if "nanolance_s" in r)
        speed = f"  time {ours:.2f} s vs pylance {theirs:.2f} s" if RANK[worst["outcome"]] <= 1 else ""
        print(f"  {worst['outcome']:8} {where}{speed}")
    print("\n" + ", ".join(f"{v} {k}" for k, v in sorted(counts.items(), key=lambda kv: RANK.get(kv[0], 9))))
    return counts


# ── Main ─────────────────────────────────────────────────────────────────────────────────────────

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="Lance datasets on the Hub")
    p.add_argument("--search")
    p.add_argument("--max-gb", type=float)
    sub.add_parser("suggested", help="the curated set")
    for name in ("fetch", "run"):
        p = sub.add_parser(name)
        if name == "fetch":
            p.add_argument("ids", nargs="+")
            p.add_argument("--only", nargs="*", help="tables (paths in the repo) to fetch, e.g. data/dev_clean.lance")
        p.add_argument("--dest", required=True, type=Path)
        p.add_argument("--max-gb", type=float, default=1.0, help="data files per table, in GB (default 1)")
        p.add_argument("--indices", action="store_true", help="also fetch _indices/")
        p.add_argument("--jobs", type=int, default=4)
    p = sub.add_parser("rewrite", help="pylance writes the tables under DIR again in another format version")
    p.add_argument("dir")
    p.add_argument("--dest", required=True, type=Path)
    p.add_argument("--version", default="2.2", help="Lance file format to write: 2.0, 2.1 or 2.2 (default 2.2)")
    p.add_argument("--rows", type=int, default=20_000, help="rows of each table to rewrite (default 20000)")
    p.add_argument("--rows-per-file", type=int, default=7_000, help="rows per data file (default 7000)")
    for name in ("check", "run"):
        p = sub.choices.get(name) or sub.add_parser(name)
        if name == "check":
            p.add_argument("dir")
        p.add_argument("--rows", type=int, default=5000)
        p.add_argument("--takes", type=int, default=256)
        p.add_argument("--files", type=int, default=3, help="data files per table read with LanceFileReader")
        p.add_argument("--full", action="store_true", help="read every fragment of complete tables")
        p.add_argument("--repeat", type=int, default=2, help="runs per check; the fastest is kept")
        p.add_argument("--json", type=Path)
        p.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    if args.cmd == "list":
        rows = hub_list(args.search)
        rows.sort(key=lambda r: r.get("mainSize") or 0)
        for r in rows:
            gb = (r.get("mainSize") or 0) / 1e9
            if args.max_gb is not None and gb > args.max_gb:
                continue
            mods = ",".join(t[9:] for t in r.get("tags", []) if t.startswith("modality:"))
            print(f"{gb:10.2f} GB  {r.get('downloads', 0):7} dl  {r['id']:60} {mods}")
        return 0
    if args.cmd == "suggested":
        for repo, only, what in SUGGESTED:
            print(f"{repo:45} {what}" + (f"  [{', '.join(only)}]" if only else ""))
        return 0
    if args.cmd == "rewrite":
        return 0 if rewrite(args) else 1
    if args.cmd in ("fetch", "run"):
        todo = [(i, args.only) for i in args.ids] if args.cmd == "fetch" else [(r, o) for r, o, _ in SUGGESTED]
        for repo, only in todo:
            try:
                fetch(repo, args.dest, args.max_gb, only, args.indices, args.jobs)
            except Exception as e:  # noqa: BLE001 -- one repo failing does not stop the rest
                print(f"{repo}: fetch failed: {type(e).__name__}: {e}", file=sys.stderr)
        if args.cmd == "fetch":
            return 0
        args.dir = args.dest
    global REPEAT
    REPEAT = max(1, args.repeat)
    results = check(args)
    counts = summarize(results)
    if args.json:
        args.json.write_text(json.dumps(results, indent=1))
    return 1 if counts.get("mismatch") else 0


if __name__ == "__main__":
    sys.exit(main())
