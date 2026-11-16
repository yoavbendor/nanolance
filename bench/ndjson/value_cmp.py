# Compare the tables two ndjson output directories both wrote (matched by column set): values row by row, in order.
# Usage: python3 value_cmp.py TOOL_OUT_DIR BASELINE_OUT_DIR   (see README "Types and values")
import lance, os, glob, sys, json
from collections import Counter

def load(side):
    res = {}
    for p in sorted(glob.glob(os.path.join(side, "*.lance"))):
        if os.path.basename(p).startswith("_"):
            continue
        d = lance.dataset(p)
        res.setdefault("|".join(sorted(f.name for f in d.schema)), []).append(p)
    return res

A = load(sys.argv[1]); B = load(sys.argv[2])
examples = []
for key in sorted(set(A) & set(B)):
    pa_path, pb_path = A[key][0], B[key][0]
    ta = lance.dataset(pa_path).to_table()
    tb = lance.dataset(pb_path).to_table().select(ta.column_names)
    # Field order: first-seen order of the keys differs when input lines shuffle key order.
    ra, rb = ta.to_pylist(), tb.to_pylist()
    equal = 0; diff_rows = 0; diff_cols = Counter(); ex = []
    for i, (x, y) in enumerate(zip(ra, rb)):
        if x == y:
            equal += 1
            continue
        diff_rows += 1
        for k in x:
            if x[k] != y.get(k):
                diff_cols[k] += 1
                if len(ex) < 5:
                    ex.append((os.path.basename(pa_path), i, k, repr(x[k])[:120], repr(y.get(k))[:120]))
    print(f"{os.path.basename(pa_path)} vs {os.path.basename(pb_path)}: rows compared {len(ra)} vs {len(rb)}, "
          f"equal {equal}, differing {diff_rows}, differing columns {dict(diff_cols)}")
    examples.extend(ex)
    del ta, tb, ra, rb
print("examples (up to 5 per table shown when any):")
for e in examples[:20]:
    print(" ", e)
