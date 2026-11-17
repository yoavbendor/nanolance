# Compare the tables two ndjson output directories both wrote (matched by column set): schemas field by field.
# Usage: python3 schema_cmp.py TOOL_OUT_DIR BASELINE_OUT_DIR   (see README "Types and values")
import lance, os, glob, sys
import pyarrow as pa

def fields(t, prefix=""):
    out = []
    for f in t:
        out.append((prefix + f.name, f.type, f.nullable))
        out.extend(children(f.type, prefix + f.name + "."))
    return out

def children(typ, prefix):
    out = []
    if pa.types.is_struct(typ):
        for i in range(typ.num_fields):
            f = typ.field(i)
            out.append((prefix + f.name, f.type, f.nullable))
            out.extend(children(f.type, prefix + f.name + "."))
    elif pa.types.is_list(typ) or pa.types.is_large_list(typ):
        f = typ.value_field
        out.append((prefix + "item", f.type, f.nullable))
        out.extend(children(f.type, prefix + "item."))
    elif pa.types.is_map(typ):
        out.append((prefix + "key", typ.key_type, False))
        out.append((prefix + "item", typ.item_type, True))
    return out

def tables(side):
    res = {}
    for p in sorted(glob.glob(os.path.join(side, "*.lance"))):
        b = os.path.basename(p)
        if b.startswith("_"):
            continue
        d = lance.dataset(p)
        key = "|".join(sorted(f.name for f in d.schema))
        res.setdefault(key, []).append((p, d.schema, d.count_rows()))
    return res

A = tables(sys.argv[1]); B = tables(sys.argv[2])
from collections import Counter
for key in sorted(set(A) & set(B)):
    for (pa_, sa, na), (pb, sb, nb) in zip(A[key], B[key]):
        if na != nb:
            print("ROWS DIFFER", pa_, pb)
    ta = [(os.path.basename(p), s) for p, s, n in A[key]]
    tb = [(os.path.basename(p), s) for p, s, n in B[key]]
    sa, sb = A[key][0][1], B[key][0][1]
    fa, fb = fields(sa), fields(sb)
    print(f"== {os.path.basename(A[key][0][0])} vs {os.path.basename(B[key][0][0])}: "
          f"{len(fa)} fields tool, {len(fb)} baseline; top-level order {'same' if sa.names == sb.names else 'DIFFERENT'}")
    diffs = []
    for i in range(max(len(fa), len(fb))):
        x = fa[i] if i < len(fa) else None
        y = fb[i] if i < len(fb) else None
        if x is None or y is None or x[0] != y[0] or str(x[1]) != str(y[1]) or x[2] != y[2]:
            diffs.append((i, x, y))
    if not diffs:
        print("   all fields identical (name, type, nullable, position)")
    for i, x, y in diffs:
        print("   pos", i, "tool:", x and (x[0], str(x[1]), "nullable" if x[2] else "not-null"),
              "| baseline:", y and (y[0], str(y[1]), "nullable" if y[2] else "not-null"))
