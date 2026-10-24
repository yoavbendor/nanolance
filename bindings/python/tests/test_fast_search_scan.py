# SPDX-License-Identifier: Apache-2.0
"""fast_search on a filtered scan reads only the fragments the scalar index it uses covers, as pylance does:
when the filter has an index expression (an indexed condition at its top level, alone or in an AND);
otherwise every fragment, as without fast_search. Scans and counts compared with pylance."""

import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl

FILTERS = ["filter >= 95", "other >= 95", "filter >= 95 AND other >= 0", "filter >= 95 OR other >= 105",
           "filter < 3 OR filter >= 108"]


@pytest.fixture()
def paths(tmp_path):
    lance = require_pylance()
    out = {}
    for tag, engine in (("py", lance), ("nl", nl)):
        path = str(tmp_path / f"{tag}.lance")
        engine.write_dataset(pa.table({"filter": range(100), "other": range(100)}), path, max_rows_per_file=100)
        engine.dataset(path).create_scalar_index("filter", "BTREE")
        engine.write_dataset(pa.table({"filter": range(100, 110), "other": range(100, 110)}), path, mode="append")
        out[tag] = (engine, path)
    return out


@pytest.mark.parametrize("f", FILTERS)
def test_fast_search_scan_as_pylance(paths, f):
    got = {}
    for tag, (engine, path) in paths.items():
        ds = engine.dataset(path)
        got[tag] = (sorted(ds.to_table(filter=f, fast_search=True)["filter"].to_pylist()),
                    ds.scanner(filter=f, fast_search=True).count_rows(),
                    ds.to_table(filter=f).num_rows)
    assert got["nl"] == got["py"]
