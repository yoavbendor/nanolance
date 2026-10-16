"""``order_by`` in scans, against pylance: per-key direction and null placement, ties broken by
the next key, NaN, filters, offset and limit after the sort, fragments, sort columns not projected."""

from __future__ import annotations

import math
import random
from datetime import datetime, timedelta

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance.dataset import ColumnOrdering
from tests.support import require_pylance


def _data(n=300, seed=7):
    rng = random.Random(seed)

    def maybe(v):
        return None if rng.random() < 0.15 else v

    return pa.table({
        "i": [maybe(rng.randint(-5, 5)) for _ in range(n)],
        "f": [maybe(rng.choice([math.nan, -0.0, 0.0, 1.5, -2.25, math.inf, -math.inf])) for _ in range(n)],
        "s": [maybe(rng.choice(["", "a", "B", "b", "ä", "aa", "z"])) for _ in range(n)],
        "b": [maybe(rng.random() < 0.5) for _ in range(n)],
        "t": pa.array([maybe(datetime(2026, 1, 1) + timedelta(hours=rng.randint(0, 9))) for _ in range(n)],
                      pa.timestamp("us")),
        "id": list(range(n)),
    })


KEYS = ["i", "f", "s", "b", "t"]


def _same(a: pa.Table, b: pa.Table) -> bool:
    """Equal tables, NaN equal to NaN (Table.equals says it is not)."""
    def norm(t):
        return {c: ["nan" if isinstance(v, float) and math.isnan(v) else v for v in t.column(c).to_pylist()]
                for c in t.column_names}
    return a.schema == b.schema and norm(a) == norm(b)


def _their_ordering():
    import importlib

    return importlib.import_module("lance.dataset").ColumnOrdering


@pytest.mark.parametrize("seed", range(12))
def test_order_by_matches_pylance(tmp_path, seed):
    lance = require_pylance()
    rng = random.Random(seed)
    data = _data(seed=seed)
    ours = nl.write_dataset(data, str(tmp_path / "n.lance"), max_rows_per_file=70)
    theirs = lance.write_dataset(data, str(tmp_path / "p.lance"), max_rows_per_file=70)
    keys = rng.sample(KEYS, rng.randint(1, 3)) + ["id"]  # id last: a total order, so ties cannot differ
    spec = [(k, rng.random() < 0.5, rng.random() < 0.5) for k in keys]
    columns = rng.choice([None, ["id"], ["s", "id", "f"]])
    kw = dict(columns=columns, filter=rng.choice([None, "i > -3", "s IS NOT NULL"]),
              offset=rng.choice([None, 0, 13]), limit=rng.choice([None, 1, 40]))
    mine = ours.to_table(order_by=[ColumnOrdering(k, a, n) for k, a, n in spec], **kw)
    try:
        pylance_ = theirs.to_table(order_by=[_their_ordering()(k, a, n) for k, a, n in spec], **kw)
    except ValueError as exc:  # pylance's planner refuses some sort/projection mixes (a TakeExec bug)
        assert "TakeExec" in str(exc)
        pylance_ = theirs.to_table(order_by=[_their_ordering()(k, a, n) for k, a, n in spec],
                                   **dict(kw, columns=None)).select(mine.column_names)
    assert _same(mine, pylance_), spec


def test_order_by_forms_and_fragments(tmp_path):
    lance = require_pylance()
    data = _data(60)
    ours = nl.write_dataset(data, str(tmp_path / "n.lance"), max_rows_per_file=25)
    theirs = lance.write_dataset(data, str(tmp_path / "p.lance"), max_rows_per_file=25)
    for order in (["i", "id"], ["s", ColumnOrdering("id", False)], ["f", "id"]):
        their_order = [_their_ordering()(o.column_name, o.ascending, o.nulls_first)
                       if isinstance(o, ColumnOrdering) else o for o in order]
        assert _same(ours.to_table(order_by=order), theirs.to_table(order_by=their_order))
        assert _same(pa.Table.from_batches(list(ours.to_batches(order_by=order, batch_size=7))),
                     theirs.to_table(order_by=their_order))
        # (pylance's fragment scanner takes ColumnOrdering only.)
        their_fragment_order = [_their_ordering()(o) if isinstance(o, str) else o for o in their_order]
        assert _same(ours.get_fragment(1).to_table(order_by=order),
                     theirs.get_fragment(1).to_table(order_by=their_fragment_order))


def test_order_by_unknown_column(tmp_path):
    ds = nl.write_dataset(_data(10), str(tmp_path / "n.lance"))
    for name in ("nope", "I"):  # names are exact, as in Lance
        with pytest.raises(ValueError, match="not found"):
            ds.to_table(order_by=[name])
