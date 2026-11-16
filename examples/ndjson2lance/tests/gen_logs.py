#!/usr/bin/env python3
"""Seeded, deterministic generator of a mixed JSON-lines log stream for ndjson2lance.

Usage:
  gen_logs.py --seed S [--lines N] [--profile small|realistic] [--kinds K]
              [--long-line-bytes B] [--out FILE] [--truth FILE]

Output is NDJSON (UTF-8, "\\n" line ends) to --out or stdout. The truth file (JSON)
goes to --truth. Same arguments give byte-identical output and truth. Standard
library only.

--lines counts records: valid objects, non-object lines and malformed lines. Blank
lines (empty or whitespace-only) are added on top, about one per 100 records, and
are listed in the truth file, not in per_line. Defaults: small 12000, realistic
50000. The small default is above "about 5,000" on purpose: case 6 needs more than
10,000 rows in one heap, so the smallest usable stream is about 11,300 lines.
--kinds K is the number of event kinds, at least 6 (the first six are the case
carriers; K > 6 adds filler kinds batch_job_N). Realistic lines are 200-2000 bytes;
the long lines are --long-line-bytes (default 20000 small, 100000 realistic).

Stream layout
  Metric kind ("metric") gets max(10200, 1/4 of the object records) rows, the other
  kinds share the rest equally. Record kinds are shuffled across the stream.

Cases (tag names used in the truth file)
  key_order_shuffle        1/3 of object lines list their top-level keys in a random
                           order, never the declared one. Same fingerprint.
  optional_key             2  auth_event has "mfa_method" in about 30% of its lines.
                           Routing by key set gives two heaps; --route-key=event gives
                           one heap, where a key first seen after the lock is skipped.
  empty_array              3  http_request rows 1..150 of the kind have "tags": [].
  array_filled                Later http_request rows always have 1-2 strings.
  always_null_key          4  db_query "replica_lag" is null in every line.
  int_widened_to_float     5  db_query "duration_ms" is an integer, except at kind rows
                           21 and 58, where it is a float (inside the first 100 rows).
  int_locked_then_float    6  metric "value" is an integer; at metric rows 10001,
                           10021, ... (every 20th from 10001) it is a float -> reject.
  type_drift_after_lock    7  metric "sample_count" is a number; at metric rows 10011,
                           10036, ... (every 25th from 10011) it is a string -> reject.
                           A row at both points gets only the case-6 float.
  non_object               8  Bare array, number, string and null lines, about one per
  malformed                   500 records, plus 1 + N/25000 truncated JSON lines.
                           Both reject in every routing mode.
  unicode                  9  Any line with non-ASCII text (Hebrew, CJK, emoji) in a
  special_key                 value or a key. special_key: a top-level key with a dot or
                           a space ("user.id", "client ip", "involved object").
  long_line               10  Error rows chosen at random get a "message" padded to
                           exactly --long-line-bytes bytes.
  (blank lines)           11  Listed in blank_line_numbers; not in per_line.

Expect rules (computed by simulate(), not hand-written)
  A heap is the routing value of a line: by key set (the sorted top-level keys joined
  with "\\u001f") or by --route-key=event (the event value). Rows of a heap are
  counted in file order, rejected lines included.
  Learning: rows 1..100 are learned. After row 100, learning goes on while the schema
  still has null-typed leaves (a null, or a list of nulls, which includes an empty
  list), up to row 10000. The lock row L is the first row r >= 100 after which no
  null leaf remains, capped at 10000. If the heap ends first, L is its row count.
  Rows 1..L are "table". A row after L is "table" if each present known key fits the
  locked type, and "reject" otherwise. Fit: null fits anything; int64 takes integers;
  float64 takes any number; utf8 takes strings; bool takes booleans; a list takes
  elements that fit its element type; a struct takes objects whose known keys fit.
  Unknown keys are skipped; missing keys read as null.
  Non-object and malformed lines are "reject" in both modes (no heap).
  The generator asserts that the simulated rejects are exactly the lines carrying
  case 6, case 7, case 8, so the design and the rules cannot drift apart silently.

Truth file (JSON)
  {
    "profile", "seed", "kinds",
    "lines": non-blank records,
    "blank_lines": n, "blank_line_numbers": [1-based physical numbers],
    "learn_rows": 100, "learn_max_rows": 10000, "long_line_bytes": B,
    "lock_points": {
      "by_keys":  {fingerprint: {"rows": n, "lock_row": L}},
      "by_event": {event value: {"rows": n, "lock_row": L}}},
    "per_line": [{
      "line": 1-based physical line number,
      "kind": event value or null,
      "fingerprint": sorted keys joined with "\\u001f", or null,
      "cases": [case tags],
      "expect_by_keys": "table" | "reject",
      "expect_by_event": "table" | "reject"}, ...]
  }
"""

import argparse
import json
import random
import sys
from datetime import datetime, timedelta, timezone

LEARN_ROWS = 100
LEARN_MAX_ROWS = 10_000
EARLY_ARRAY_ROWS = 150
DB_FLOAT_ROWS = (21, 58)
METRIC_MIN_ROWS = 10_200
METRIC_FLOAT_FIRST = 10_001
METRIC_FLOAT_STEP = 20
METRIC_DRIFT_FIRST = 10_011
METRIC_DRIFT_STEP = 25
MIN_OTHER_ROWS = 200
REJECT_CASES = frozenset(
    {"non_object", "malformed", "int_locked_then_float", "type_drift_after_lock"}
)
PROFILES = {
    "small": {"lines": 12_000, "long_bytes": 20_000, "long_count": 2},
    "realistic": {"lines": 50_000, "long_bytes": 100_000, "long_count": 3},
}
BASE_KINDS = ["http_request", "db_query", "auth_event", "metric", "k8s_event", "error"]
BASE_TS = datetime(2026, 1, 1, tzinfo=timezone.utc)
NULL = ("null",)

WORDS = ["alpha", "beta", "gamma", "delta", "request", "upstream", "cache", "miss",
         "hit", "retry", "timeout", "pool", "shard", "replica"]
SERVICES = ["gateway", "auth-svc", "billing", "search", "שירות-תשלום", "用户服务"]
PATHS = ["/api/v1/items", "/api/v1/users/42", "/שלום/עולם", "/用户/资料", "/launch/🚀"]
METHODS = ["GET", "POST", "PUT", "DELETE"]
REGIONS = ["eu-west-1", "us-east-1", "il-central-1"]
ROLES = ["admin", "reader", "writer", "auditor"]
NAMES = ["alice", "bob", "משתמש-7", "用户-8"]
REASONS = ["Pulled", "Started", "BackOff", "Killing", "🚀Scheduled"]
NONOBJECTS = [[1, 2, 3], 42, "just a string", None]


# ---------------------------------------------------------------- schema model

class SchemaConflict(ValueError):
    pass


def infer(value):
    if value is None:
        return NULL
    if isinstance(value, bool):
        return ("bool",)
    if isinstance(value, int):
        return ("int64",)
    if isinstance(value, float):
        return ("float64",)
    if isinstance(value, str):
        return ("utf8",)
    if isinstance(value, dict):
        return ("struct", {k: infer(v) for k, v in value.items()})
    elem = NULL
    for v in value:
        elem = merge(elem, infer(v))
    return ("list", elem)


def merge(a, b):
    if a == NULL:
        return b
    if b == NULL:
        return a
    if a[0] == "struct" and b[0] == "struct":
        fields = dict(a[1])
        for k, t in b[1].items():
            fields[k] = merge(fields[k], t) if k in fields else t
        return ("struct", fields)
    if a[0] == "list" and b[0] == "list":
        return ("list", merge(a[1], b[1]))
    if {a[0], b[0]} == {"int64", "float64"}:
        return ("float64",)
    if a == b:
        return a
    raise SchemaConflict(f"cannot merge {a} with {b}")


def has_null(t):
    if t == NULL:
        return True
    if t[0] == "struct":
        return any(has_null(x) for x in t[1].values())
    if t[0] == "list":
        return has_null(t[1])
    return False


def fits(t, value):
    if value is None:
        return True
    kind = t[0]
    if kind == "null":
        return False
    if kind == "bool":
        return isinstance(value, bool)
    if kind == "int64":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "float64":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if kind == "utf8":
        return isinstance(value, str)
    if kind == "struct":
        return isinstance(value, dict) and all(
            fits(t[1][k], v) for k, v in value.items() if k in t[1]
        )
    return isinstance(value, list) and all(fits(t[1], v) for v in value)


def simulate(heaps):
    """heaps: {heap: [(line, value), ...]} in file order.

    Returns ({line: "table" | "reject"}, {heap: {"rows": n, "lock_row": L}}).
    """
    expect, locks = {}, {}
    for heap, rows in heaps.items():
        schema, lock = None, len(rows)
        for r, (_, value) in enumerate(rows, 1):
            t = infer(value)
            schema = t if schema is None else merge(schema, t)
            if r == LEARN_MAX_ROWS or (r >= LEARN_ROWS and not has_null(schema)):
                lock = r
                break
        locks[heap] = {"rows": len(rows), "lock_row": lock}
        for r, (line, value) in enumerate(rows, 1):
            expect[line] = "table" if r <= lock or fits(schema, value) else "reject"
    return expect, locks


# ---------------------------------------------------------------- record building

def _dumps(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _size(value):
    return len(_dumps(value).encode("utf-8"))


def _ts(i, rng):
    moment = BASE_TS + timedelta(milliseconds=i * 250 + rng.randint(0, 249))
    return moment.isoformat(timespec="milliseconds")


def _words(rng, n):
    """ASCII text of exactly n characters (n >= 0)."""
    text = ""
    while len(text) < n:
        text = f"{text} {rng.choice(WORDS)}" if text else rng.choice(WORDS)
    return text[:n]


def b_http(krow, ts, rng):
    tags = []
    if krow > EARLY_ARRAY_ROWS:
        tags = [rng.choice(WORDS)] + ([rng.choice(WORDS)] if rng.random() < 0.5 else [])
    return {
        "event": "http_request",
        "ts": ts,
        "service": rng.choice(SERVICES),
        "method": rng.choice(METHODS),
        "path": rng.choice(PATHS),
        "status": rng.choice([200, 201, 204, 404, 500]),
        "duration_ms": rng.randint(1, 900),
        "client ip": f"10.{rng.randint(0, 255)}.{rng.randint(0, 255)}.{rng.randint(1, 254)}",
        "user.id": f"u-{rng.randint(1, 99999)}",
        "tags": tags,
        "hops": [{"host": f"h{rng.randint(1, 20)}", "ms": rng.randint(1, 50)}
                 for _ in range(rng.randint(1, 3))],
    }, {}


def b_db(krow, ts, rng):
    flags = {}
    duration = rng.randint(1, 500)
    if krow in DB_FLOAT_ROWS:
        duration = duration + 0.5
        flags["widened"] = True
    return {
        "event": "db_query",
        "ts": ts,
        "db": rng.choice(["orders", "users", "billing"]),
        "query_hash": f"{rng.getrandbits(64):016x}",
        "rows": rng.randint(0, 5000),
        "duration_ms": duration,
        "replica_lag": None,
        "shard": {
            "id": rng.randint(0, 15),
            "region": rng.choice(REGIONS),
            "node": {"name": f"db-{rng.randint(1, 9)}", "zone": rng.choice(["a", "b", "c"])},
        },
    }, flags


def b_auth(krow, ts, rng):
    obj = {
        "event": "auth_event",
        "ts": ts,
        "outcome": rng.choice(["ok", "denied", "locked"]),
        "actor": {
            "name": rng.choice(NAMES),
            "roles": rng.sample(ROLES, rng.randint(1, 3)),
            "session": {"id": f"s-{rng.getrandbits(32):08x}",
                        "ip": f"192.168.{rng.randint(0, 255)}.{rng.randint(1, 254)}"},
        },
    }
    if rng.random() < 0.3:
        obj["mfa_method"] = rng.choice(["totp", "sms", "webauthn"])
    return obj, {}


def b_metric(krow, ts, rng):
    flags = {}
    value = rng.randint(0, 1000)
    sample = rng.randint(1, 500)
    if krow >= METRIC_FLOAT_FIRST and (krow - METRIC_FLOAT_FIRST) % METRIC_FLOAT_STEP == 0:
        value = value + 0.25
        flags["locked_float"] = True
    elif krow >= METRIC_DRIFT_FIRST and (krow - METRIC_DRIFT_FIRST) % METRIC_DRIFT_STEP == 0:
        sample = str(sample)
        flags["drift"] = True
    return {
        "event": "metric",
        "ts": ts,
        "name": rng.choice(["latency.p99", "cpu.util", "queue.depth"]),
        "value": value,
        "sample_count": sample,
        "labels": {"region": rng.choice(REGIONS), "host": f"node-{rng.randint(1, 40)}"},
        "buckets": [rng.randint(0, 100) for _ in range(rng.randint(1, 5))],
    }, flags


def b_k8s(krow, ts, rng):
    return {
        "event": "k8s_event",
        "ts": ts,
        "namespace": rng.choice(["default", "prod", "staging"]),
        "reason": rng.choice(REASONS),
        "involved object": {"kind": "Pod", "name": f"pod-{rng.randint(1, 500)}",
                            "uid": f"{rng.getrandbits(48):012x}"},
        "count": rng.randint(1, 20),
        "conditions": [rng.choice(["Ready", "Scheduled", "Initialized"])
                       for _ in range(rng.randint(1, 3))],
    }, {}


def b_error(krow, ts, rng):
    return {
        "event": "error",
        "ts": ts,
        "level": rng.choice(["error", "fatal", "warn"]),
        "message": f"upstream failed after {rng.randint(1, 9)} retries",
        "stack": [f"frame_{j}" for j in range(rng.randint(1, 4))],
        "code": rng.randint(100, 599),
        "context": {
            "שגיאה": rng.choice(["חריגה", "פסק זמן"]),
            "retry": rng.randint(0, 5),
            "where": {"file": rng.choice(["app.py", "שרת/main.py", "svc/用户.py"]),
                      "line": rng.randint(1, 900)},
        },
    }, {}


def _batch_builder(name):
    def build(krow, ts, rng):
        return {
            "event": name,
            "ts": ts,
            "job": f"job-{rng.randint(1, 99)}",
            "items": rng.randint(0, 1000),
            "ok": rng.random() < 0.9,
        }, {}
    return build


def _kind_table(kinds):
    table = {"http_request": b_http, "db_query": b_db, "auth_event": b_auth,
             "metric": b_metric, "k8s_event": b_k8s, "error": b_error}
    names = list(BASE_KINDS)
    for j in range(1, kinds - len(BASE_KINDS) + 1):
        name = f"batch_job_{j}"
        names.append(name)
        table[name] = _batch_builder(name)
    return names, table


def finalize(obj, flags, rng, long_bytes, realistic):
    """Adds "detail" (last key) and, for long-line rows, pads "message" to long_bytes."""
    obj["detail"] = ""
    if flags.get("long"):
        obj["message"] = ""
        obj["message"] = "x" * (long_bytes - _size(obj))
        return
    if realistic:
        obj["detail"] = _words(rng, max(0, rng.randint(200, 2000) - _size(obj)))
    else:
        obj["detail"] = _words(rng, rng.randint(0, 40))


def generate(profile, lines, seed, kinds, long_bytes):
    """Returns (physical, n_blank); physical is [(text, record-or-None)] in file order."""
    cfg = PROFILES[profile]
    realistic = profile == "realistic"
    names, builders = _kind_table(kinds)
    rng = random.Random(seed)

    n_nonobj = max(4, lines // 500)
    n_malformed = 1 + lines // 25_000
    n_obj = lines - n_nonobj - n_malformed
    metric_n = max(METRIC_MIN_ROWS, n_obj // 4)
    others = n_obj - metric_n
    non_metric = [n for n in names if n != "metric"]
    if others < len(non_metric) * MIN_OTHER_ROWS:
        raise ValueError(
            f"--lines {lines} is too small for --kinds {kinds}; "
            f"needs at least {METRIC_MIN_ROWS + MIN_OTHER_ROWS * len(non_metric) + 50}"
        )
    counts = {"metric": metric_n}
    base, rem = divmod(others, len(non_metric))
    for j, name in enumerate(non_metric):
        counts[name] = base + (1 if j < rem else 0)
    seq = [name for name in names for _ in range(counts[name])]
    rng.shuffle(seq)

    special_kinds = ["malformed"] * n_malformed + ["non_object"] * n_nonobj
    rng.shuffle(special_kinds)
    special = dict(zip(sorted(rng.sample(range(lines), len(special_kinds))), special_kinds))
    long_rows = set(rng.sample(range(1, counts["error"] + 1), cfg["long_count"]))

    records = []
    seen = {name: 0 for name in names}
    nonobj_i = 0
    oi = 0
    for i in range(lines):
        if i in special:
            if special[i] == "malformed":
                text = f'{{"event":"db_query","ts":"{_ts(i, rng)}","rows":3'
            else:
                text = _dumps(NONOBJECTS[nonobj_i % len(NONOBJECTS)])
                nonobj_i += 1
            records.append({"text": text, "kind": None, "krow": None, "flags": {}})
            continue
        kind = seq[oi]
        oi += 1
        seen[kind] += 1
        krow = seen[kind]
        obj, flags = builders[kind](krow, _ts(i, rng), rng)
        if kind == "error" and krow in long_rows:
            flags["long"] = True
        finalize(obj, flags, rng, long_bytes, realistic)
        if rng.random() < 1 / 3:
            keys = list(obj)
            perm = keys[:]
            while perm == keys:
                rng.shuffle(perm)
            obj = {k: obj[k] for k in perm}
            flags["shuffled"] = True
        records.append({"text": _dumps(obj), "kind": kind, "krow": krow, "flags": flags})

    n_blank = max(1, lines // 100)
    blank_at = set(rng.sample(range(lines + n_blank), n_blank))
    physical = []
    it = iter(records)
    for p in range(lines + n_blank):
        if p in blank_at:
            physical.append((rng.choice(["", "   "]), None))
        else:
            rec = next(it)
            physical.append((rec["text"], rec))
    return physical, n_blank


def case_tags(text, value, parse_ok, is_obj, rec):
    if not parse_ok:
        return ["malformed"]
    if not is_obj:
        return ["non_object"]
    flags, cases = rec["flags"], []
    if flags.get("shuffled"):
        cases.append("key_order_shuffle")
    if "mfa_method" in value:
        cases.append("optional_key")
    if value.get("tags") == []:
        cases.append("empty_array")
    elif "tags" in value and rec["krow"] > EARLY_ARRAY_ROWS:
        cases.append("array_filled")
    if "replica_lag" in value and value["replica_lag"] is None:
        cases.append("always_null_key")
    if flags.get("widened"):
        cases.append("int_widened_to_float")
    if flags.get("locked_float"):
        cases.append("int_locked_then_float")
    if flags.get("drift"):
        cases.append("type_drift_after_lock")
    if any("." in k or " " in k for k in value):
        cases.append("special_key")
    if any(ord(c) > 127 for c in text):
        cases.append("unicode")
    if flags.get("long"):
        cases.append("long_line")
    return cases


def build_truth(physical, seed, profile, kinds, long_bytes, n_blank):
    nonblank, blank_numbers = [], []
    for p, (text, rec) in enumerate(physical, 1):
        if rec is None:
            blank_numbers.append(p)
        else:
            nonblank.append((p, text, rec))

    parsed, heaps_k, heaps_e = [], {}, {}
    for p, text, rec in nonblank:
        try:
            value, parse_ok = json.loads(text), True
        except ValueError:
            value, parse_ok = None, False
        is_obj = parse_ok and isinstance(value, dict)
        fp = event = None
        if is_obj:
            fp = "\x1f".join(sorted(value))
            event = value["event"]
            heaps_k.setdefault(fp, []).append((p, value))
            heaps_e.setdefault(event, []).append((p, value))
        parsed.append((p, text, rec, value, parse_ok, is_obj, fp, event))

    expect_k, locks_k = simulate(heaps_k)
    expect_e, locks_e = simulate(heaps_e)

    per_line = []
    for p, text, rec, value, parse_ok, is_obj, fp, event in parsed:
        cases = case_tags(text, value, parse_ok, is_obj, rec)
        if is_obj:
            ek, ee = expect_k[p], expect_e[p]
        else:
            ek = ee = "reject"
        planned = bool(REJECT_CASES & set(cases))
        if (ek == "reject") != planned or (ee == "reject") != planned:
            raise RuntimeError(
                f"line {p}: simulated expect {ek}/{ee} disagrees with cases {cases}"
            )
        per_line.append({
            "line": p,
            "kind": event,
            "fingerprint": fp,
            "cases": cases,
            "expect_by_keys": ek,
            "expect_by_event": ee,
        })

    return {
        "profile": profile,
        "seed": seed,
        "kinds": kinds,
        "lines": len(nonblank),
        "blank_lines": n_blank,
        "blank_line_numbers": blank_numbers,
        "learn_rows": LEARN_ROWS,
        "learn_max_rows": LEARN_MAX_ROWS,
        "long_line_bytes": long_bytes,
        "lock_points": {"by_keys": locks_k, "by_event": locks_e},
        "per_line": per_line,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Seeded mixed JSON-lines log generator.")
    parser.add_argument("--lines", type=int, default=None,
                        help="non-blank records (default: 12000 small, 50000 realistic)")
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--out", help="NDJSON output file (default: stdout)")
    parser.add_argument("--truth", help="truth JSON file")
    parser.add_argument("--kinds", type=int, default=6, help="event kinds, at least 6")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="small")
    parser.add_argument("--long-line-bytes", type=int, default=None)
    args = parser.parse_args(argv)

    if args.kinds < len(BASE_KINDS):
        parser.error(f"--kinds must be at least {len(BASE_KINDS)}")
    cfg = PROFILES[args.profile]
    lines = args.lines if args.lines is not None else cfg["lines"]
    long_bytes = args.long_line_bytes or cfg["long_bytes"]
    try:
        physical, n_blank = generate(args.profile, lines, args.seed, args.kinds, long_bytes)
    except ValueError as exc:
        parser.error(str(exc))
    truth = build_truth(physical, args.seed, args.profile, args.kinds, long_bytes, n_blank)

    data = "".join(text + "\n" for text, _ in physical).encode("utf-8")
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
    else:
        sys.stdout.buffer.write(data)
    if args.truth:
        with open(args.truth, "w", encoding="utf-8") as f:
            json.dump(truth, f, ensure_ascii=False)
            f.write("\n")


if __name__ == "__main__":
    main()
