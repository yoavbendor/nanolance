"""String filters on text that is not ASCII, answered as pylance answers them.

LIKE's _ is one character (not one byte), ILIKE and the 'i' regex flag compare Unicode case folds, lower()
and upper() change every script's case (the final sigma, ß -> SS, İ -> i̇), and regexp_like's ., ranges, \\w,
\\d, \\s and [[:alpha:]] work on characters. Each filter runs through nanolance and pylance on the same
dataset, and the rows must agree; ASCII rows are in the same table, so the byte fast paths are covered too.
"""
from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

DOCS = [
    "שלום עולם", "שָׁלוֹם עוֹלָם", "مرحبا بالعالم", "Привет мир", "ΣΟΦΟΣ ΛΟΓΟΣ", "σοφός", "Straße", "İstanbul",
    "ÜNÏCÖDÉ façade", "Ștefan Țară", "hello world", "HELLO_World 42", "١٢٣ أرقام", "a b", "100%_done", None,
    "ספר‏ חדש", "😀 emoji", "",
]

FILTERS = [
    # LIKE: _ is one character, % any run, \ escapes.
    "t LIKE '_לום%'", "t LIKE '%עו_ם'", "t LIKE '_ривет%'", "t NOT LIKE '_ривет%'", "t LIKE '_ emoji'",
    "t LIKE '%\\%\\_%'", "t LIKE 'hello%'", "t LIKE '_ello world'", "t LIKE '%'", "t LIKE ''",
    # ILIKE: Unicode case folds; ß is not SS.
    "t ILIKE 'привет%'", "t ILIKE '%FAÇADE'", "t ILIKE 'ștefan%'", "t ILIKE 'STRASSE'", "t ILIKE 'σοφοσ%'",
    "t ILIKE 'HELLO%'", "t NOT ILIKE '%world%'",
    # lower() / upper().
    "lower(t) = 'привет мир'", "lower(t) = 'σοφος λογος'", "lower(t) = 'i̇stanbul'", "lower(t) = 'straße'",
    "upper(t) = 'STRASSE'", "upper(t) = 'ÜNÏCÖDÉ FAÇADE'", "upper(t) = 'ȘTEFAN ȚARĂ'", "upper(t) = 'HELLO WORLD'",
    "lower(t) = t", "contains(lower(t), 'мир')",
    # Regular expressions over characters.
    "regexp_like(t, '^.לום')", "regexp_like(t, '^[א-ת]+ ')", "regexp_like(t, 'ПРИВЕТ', 'i')",
    "regexp_like(t, '^\\w+$')", "regexp_like(t, '^..... ')", "regexp_like(t, '^\\d+ ')", "regexp_like(t, 'a\\sb')",
    "regexp_like(t, '^[[:alpha:]]+$')", "regexp_like(t, '^\\W')", "regexp_like(t, 'world$')",
    "regexp_like(t, '^.$')", "regexp_like(t, '^. emoji')",
    # Unaffected functions, as controls.
    "length(t) = 9", "starts_with(t, 'שָׁ')", "t = 'שלום עולם'",
]


@pytest.fixture(scope="module")
def path(tmp_path_factory):
    lance = require_pylance()
    p = str(tmp_path_factory.mktemp("unicode") / "ds.lance")
    lance.write_dataset(pa.table({"id": pa.array(range(len(DOCS)), pa.int64()), "t": pa.array(DOCS, pa.utf8())}), p)
    return p


@pytest.mark.parametrize("flt", FILTERS)
def test_filter_matches_pylance(path, flt):
    lance = require_pylance()
    want = sorted(lance.dataset(path).to_table(filter=flt).column("id").to_pylist())
    got = sorted(nl.dataset(path).to_table(filter=flt).column("id").to_pylist())
    assert got == want, f"{flt}: nanolance {got}, pylance {want}"


def test_lower_upper_values_match_pylance(path):
    """The values themselves, not just a comparison: every row's lower() and upper()."""
    lance = require_pylance()
    for flt_col in ("lower(t)", "upper(t)"):
        for i, doc in enumerate(DOCS):
            if doc is None:
                continue
            want = lance.dataset(path).to_table(filter=f"{flt_col} = t AND id = {i}").num_rows
            got = nl.dataset(path).to_table(filter=f"{flt_col} = t AND id = {i}").num_rows
            assert got == want, f"{flt_col} of {doc!r}"
