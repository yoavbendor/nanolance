# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""The pylance surface nanolance adds in docs/tasks/PY_SURFACE.md: ``lance.log`` and Lance's native log.

Each test runs the same code under real pylance and under nanolance (``install_as_lance``), in a
subprocess, and compares what they log. Timestamps are dropped; uri and file-name values are
replaced, since they differ by construction.
"""

from __future__ import annotations

import importlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest

from tests.support import require_pylance

ENGINES = ("pylance", "nanolance")
DATASET_EVENTS = "lance::events::dataset_events"
FILE_AUDIT = "lance::events::file_audit"
# bindings/python: the directory holding the nanolance package
NANOLANCE_PATH = Path(__file__).resolve().parents[1]

PRELUDE = {
    "pylance": "import lance\n",
    "nanolance": "import nanolance.lance as _nl\n_nl.install_as_lance()\nimport lance\n",
}

WRITE_OPS = (
    "import sys, pyarrow as pa\n"
    "d = sys.argv[1]\n"
    "t = pa.table({'x': range(10)})\n"
    "lance.write_dataset(t, d + '/a.lance')\n"
    "lance.write_dataset(t, d + '/a.lance', mode='append')\n"
)
# write, append, open, delete
ALL_OPS = WRITE_OPS + (
    "lance.dataset(d + '/a.lance').to_table()\n"
    "lance.dataset(d + '/a.lance').delete('x < 3')\n"
)

LOG_LINE = re.compile(r"^\[(\S+) +(\w+) +([^\]]+)\] (.*)$")


def _run(engine: str, code: str, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
    run_env = {"PATH": os.environ.get("PATH", "")}
    if engine == "nanolance":
        run_env["PYTHONPATH"] = str(NANOLANCE_PATH)
    run_env.update(env or {})
    result = subprocess.run([sys.executable, "-c", PRELUDE[engine] + code, *args],
                            capture_output=True, env=run_env, timeout=300)
    assert result.returncode == 0, f"{engine}: {result.stderr.decode(errors='replace')}"
    return result


def _events(text: bytes | str, *targets: str) -> list:
    """The log lines for ``targets`` as ``LEVEL target body``: no timestamp, uri and path replaced."""
    if isinstance(text, bytes):
        text = text.decode(errors="replace")
    out = []
    for line in text.splitlines():
        match = LOG_LINE.match(line)
        if match and match.group(3) in targets:
            body = re.sub(r'uri="[^"]*"', 'uri="U"', match.group(4))
            body = re.sub(r'path="[^"]*"', 'path="P"', body)
            out.append(f"{match.group(2)} {match.group(3)} {body}")
    return out


def _log_lines(text: bytes) -> list:
    return [line for line in text.decode(errors="replace").splitlines() if LOG_LINE.match(line)]


@pytest.mark.parametrize("env_value", ["DEBUG", "INFO", "WARNING", "DEBUG,INFO", "", "lance-core=debug,WARNING",
                                       "trace"])
def test_log_level_matches_pylance(env_value):
    require_pylance()
    lance_log = importlib.import_module("lance.log")
    nanolance_log = importlib.import_module("nanolance.lance.log")
    assert nanolance_log.ENV_NAME_PYLANCE_LOGGING_LEVEL == lance_log.ENV_NAME_PYLANCE_LOGGING_LEVEL
    with mock.patch.dict(os.environ, {"LANCE_LOG": env_value}):
        assert nanolance_log.get_log_level() == lance_log.get_log_level()


def test_write_append_open_delete_events_match_pylance(tmp_path):
    require_pylance()
    events = {}
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, ALL_OPS, str(data), env={"LANCE_LOG": "info"})
        events[engine] = _events(result.stderr, DATASET_EVENTS, FILE_AUDIT)
    # pylance also audits the deletion files and logs its plan (lance::events::execution); nanolance
    # does not log those yet, so they are left out of the comparison.
    expected = [line for line in events["pylance"] if 'type="deletion"' not in line]
    assert events["nanolance"], "nanolance logged nothing"
    assert events["nanolance"] == expected
    assert any('event="committed"' in line and 'operation="Delete"' in line for line in events["nanolance"])


@pytest.mark.parametrize("spec, expect_file_audit", [
    ("warn", False),
    ("warn,lance::events::dataset_events=info", False),
    ("info,lance::events::file_audit=off", False),
    ("info", True),
])
def test_filters_match_pylance(tmp_path, spec, expect_file_audit):
    require_pylance()
    events = {}
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": spec})
        events[engine] = _events(result.stderr, DATASET_EVENTS, FILE_AUDIT)
    assert events["nanolance"] == events["pylance"]
    has_file_audit = any(line.startswith("INFO lance::events::file_audit") for line in events["nanolance"])
    assert has_file_audit is expect_file_audit
    if spec == "warn":
        assert events["nanolance"] == []


@pytest.mark.parametrize("precision, digits", [("s", 0), ("ms", 3), ("us", 6), ("ns", 9)])
def test_timestamp_precision_matches_pylance(tmp_path, precision, digits):
    require_pylance()
    fraction = rf"\.\d{{{digits}}}" if digits else ""
    pattern = re.compile(r"^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}" + fraction + r"Z ")
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": "debug",
                                                          "LANCE_LOG_TS_PRECISION": precision})
        first = result.stderr.decode().splitlines()[0]
        assert pattern.match(first), f"{engine}: {first}"


def test_invalid_timestamp_precision_is_reported(tmp_path):
    require_pylance()
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": "info",
                                                          "LANCE_LOG_TS_PRECISION": "bogus"})
        # pylance's own warning is not captured under pytest here (it shows in a shell); nanolance's is.
        if engine == "nanolance":
            assert "Invalid timestamp precision" in result.stderr.decode()
        assert _events(result.stderr, DATASET_EVENTS), engine


def test_log_file_is_written_with_parent_directories(tmp_path):
    require_pylance()
    contents = {}
    for engine in ENGINES:
        log_file = tmp_path / engine / "nested" / "lance.log"
        result = _run(engine, WRITE_OPS, str(tmp_path / engine), env={"LANCE_LOG": "info",
                                                                       "LANCE_LOG_FILE": str(log_file)})
        assert log_file.exists(), engine
        assert _log_lines(result.stderr) == [], f"{engine} wrote log lines to stderr as well as the file"
        contents[engine] = _events(log_file.read_bytes(), DATASET_EVENTS, FILE_AUDIT)
        assert contents[engine], engine
    assert contents["nanolance"] == contents["pylance"]


def test_unwritable_log_file_is_reported_and_ignored(tmp_path):
    require_pylance()
    blocker = tmp_path / "blocker"
    blocker.write_text("a file, so it cannot be a directory")
    log_file = blocker / "lance.log"
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        # pylance prints nothing for this case; nanolance says why the log is not written.
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": "info", "LANCE_LOG_FILE": str(log_file)})
        assert not log_file.exists(), engine
        if engine == "nanolance":
            assert "cannot open LANCE_LOG_FILE" in result.stderr.decode()


def test_no_native_log_without_lance_log(tmp_path):
    result = _run("nanolance", WRITE_OPS, str(tmp_path), env={})
    assert _log_lines(result.stderr) == []


# ---------------------------------------------------------------------------------------------
# lance.arrow: bfloat16 and the image types (tests/test_arrow.py of pylance)
# ---------------------------------------------------------------------------------------------

BF16_EDGE_SCRIPT = r"""
import json, struct
import numpy as np, pyarrow as pa
from lance.arrow import BFloat16, bfloat16_array, cast

def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]

vals = [float("nan"), -float("nan"), f32(0x7F800001), f32(0xFFC00001), f32(0x7FA00000), f32(0xFFA00000),
        float("inf"), float("-inf"), -0.0, 0.0, f32(0x00000001), f32(0x00008000), f32(0x00007FFF),
        f32(0x00018000), f32(0x00010000), f32(0x80008000), f32(0x7F7FFFFF), f32(0x7F7F8000),
        f32(0x7F7F7FFF), f32(0x7F808000), f32(0x7F800000), f32(0x00800000), f32(0x007FFFFF),
        1e-40, 1e-45, 5e-324, 1.0 + 2**-8, 1.0 + 3 * 2**-8, 1.0 + 2**-8 + 2**-60, 1.0 + 2**-8 - 2**-60,
        2.0**-134, 2.0**-133, 3.3895313892515355e38, 3.4028235e38, 1e39, -1e308, 1.1, 1.124565, 3.4,
        123456.0, 0.1, 2.5e-30, 65504.0]
rng = np.random.default_rng(7)
vals += list(rng.standard_normal(400) * 10.0 ** rng.integers(-40, 38, 400))
vals += [float(x) for x in rng.integers(-2**20, 2**20, 50)]
vals += [float(x) for x in rng.random(50).astype(np.float16)]

def bits(arr):
    return arr.storage.buffers()[1].to_pybytes().hex()

out = {"scalars": [[str(BFloat16(v)), bits(bfloat16_array([v]))] for v in vals], "casts": {}}
for name, dt in (("float16", np.float16), ("float32", np.float32), ("float64", np.float64)):
    out["casts"][name] = bits(cast(pa.array(np.array(vals, dtype=np.float64).astype(dt)), "bfloat16"))
print(json.dumps(out))
"""

BF16_API_SCRIPT = r"""
import json
from lance.arrow import BFloat16, BFloat16Type, BFloat16Array, bfloat16_array, cast
import pyarrow as pa
import numpy as np
from ml_dtypes import bfloat16

def attempt(fn, message=True):
    try:
        return ["ok", repr(fn())]
    except Exception as e:  # the type always matters; the message only where pylance's is the contract
        return ["error", type(e).__name__, str(e)] if message else ["error", type(e).__name__]

out = {}
out["from_bytes"] = attempt(lambda: BFloat16.from_bytes(b"\xc0\x3f"))
out["from_bytes_len"] = [attempt(lambda n=n: BFloat16.from_bytes(b"\x00" * n)) for n in (0, 1, 3, 4)]
out["from_bytes_type"] = attempt(lambda: BFloat16.from_bytes("ab"))
out["noarg"] = attempt(lambda: BFloat16(), message=False)
out["str_arg"] = attempt(lambda: BFloat16("1.5"))
out["int_arg"] = attempt(lambda: BFloat16(3))
out["compare"] = [BFloat16(1.1) > BFloat16(1.0), BFloat16(1.1) >= BFloat16(1.0), BFloat16(1.1) != BFloat16(1.0),
                  BFloat16(1.1) < BFloat16(1.0), BFloat16(1.1) == BFloat16(1.1), BFloat16(float("nan")) == BFloat16(float("nan")),
                  BFloat16(-0.0) == BFloat16(0.0), BFloat16(1.0) == 1.0, BFloat16(1.0) != 1.0]
out["compare_float"] = attempt(lambda: BFloat16(1.0) < 1.5, message=False)
out["compare_str"] = attempt(lambda: BFloat16(1.0) < "x", message=False)
out["hash"] = attempt(lambda: hash(BFloat16(1.0)), message=False)
out["float_of"] = attempt(lambda: float(BFloat16(1.5)), message=False)
out["values"] = [str(BFloat16(1.124565)), str(BFloat16(1.1)), str(BFloat16(float("-inf"))), str(BFloat16(-0.0))]
out["type"] = [str(BFloat16Type()), BFloat16Type().extension_name, str(BFloat16Type().storage_type),
               str(BFloat16Type() == BFloat16Type()), str(BFloat16Type() == pa.binary(2))]
arr = bfloat16_array([1.1, None, 3.4])
out["array_repr"] = repr(arr).split("\n", 1)[1]
out["array_str"] = str(arr)
out["array_empty_str"] = str(bfloat16_array([]))
out["to_pylist"] = [repr(v) for v in arr.to_pylist()]
out["scalar"] = [repr(arr[0].as_py()), arr[0] == BFloat16(1.1), arr[0] == bfloat16(1.1), arr[0] == 1.1, arr[1].as_py() is None]
out["scalar_repr"] = repr(arr[0])
out["array_from_numpy"] = str(BFloat16Array.from_numpy(np.array([1.0, 2.0, 3.0], dtype=bfloat16)))
out["from_numpy_wrong"] = attempt(lambda: BFloat16Array.from_numpy(np.array([1.0], dtype=np.float32)))
out["bfloat16_str_arg"] = attempt(lambda: bfloat16_array(["x"]), message=False)
out["bfloat16_empty"] = repr(bfloat16_array([]).type)
out["slice"] = [repr(v) for v in bfloat16_array([1.0, 2.0, 3.0]).slice(1, 1).to_pylist()]
out["cast_to_int"] = attempt(lambda: cast(bfloat16_array([1.0]), pa.int32()))
out["cast_from_int"] = attempt(lambda: cast(pa.array([1, 2], pa.int32()), "bfloat16"))
out["cast_str_target"] = str(cast(pa.array([1.0], pa.float32()), "bf16").type)
out["cast_back_f16"] = [repr(v) for v in cast(bfloat16_array([1.5]), pa.float16()).to_pylist()]
out["cast_fsl_size"] = attempt(lambda: cast(pa.FixedSizeListArray.from_arrays(pa.array([1.0, 2.0, 3.0, 4.0], pa.float32()), 2), pa.list_(pa.float32(), 4)))
print(json.dumps(out, default=str))
"""

BF16_PANDAS_SCRIPT = r"""
import json
import numpy as np, pandas as pd, pyarrow as pa
from ml_dtypes import bfloat16
from lance.arrow import BFloat16Array, PandasBFloat16Array, bfloat16_array

arr = bfloat16_array([1.1, None, 3.4])
series = arr.to_pandas()
out = {
    "dtype": str(series.dtype),
    "name": series.name,
    "values": [repr(v) for v in series.tolist()],
    "isna": series.isna().tolist(),
    "series_dtype_str": str(pd.Series(series, dtype="lance.bfloat16").dtype),
    "arrow_roundtrip": [type(pa.array(series)).__name__, str(pa.array(series).type), pa.array(series) == arr],
    "pandas_roundtrip_repr": repr(pa.array(series).to_pandas().tolist()),
    "numpy_bits": BFloat16Array.from_numpy(np.array([1.0, 2.5, -0.0, np.nan], dtype=bfloat16)).storage.buffers()[1].to_pybytes().hex(),
    "numpy_dtype": str(bfloat16_array([1.0, 2.0]).to_numpy().dtype),
    "numpy_values": [repr(float(x)) for x in bfloat16_array([1.1, 2.1, 3.4]).to_numpy()],
    "numpy_bits_pandas": PandasBFloat16Array.from_numpy(np.array([1.0, np.nan], dtype=bfloat16)).to_numpy().view(np.uint16).tolist(),
    "pandas_no_null_repr": repr(pd.Series(bfloat16_array([1.0, 2.0]).to_pandas())),
    "frame_equal": pd.testing.assert_series_equal(series, pd.Series(series, dtype="lance.bfloat16")) is None,
}
print(json.dumps(out, default=str))
"""

BF16_IO_SCRIPT = r"""
import json, sys
import numpy as np, pyarrow as pa, lance
from ml_dtypes import bfloat16
from lance.arrow import BFloat16Array, bfloat16_array

mode, path = sys.argv[1], sys.argv[2]
if mode == "write":
    values = BFloat16Array.from_numpy(np.array([1.0, 1.1, -0.0, np.nan, 3.4, 65504.0, 1e-40, 2.0], dtype=bfloat16))
    vectors = pa.FixedSizeListArray.from_arrays(values, 4)  # two rows of four bfloat16
    tensors = pa.ExtensionArray.from_storage(pa.fixed_shape_tensor(values.type, [4]), vectors)
    table = pa.table({
        "id": pa.array([0, 1]),
        "values": values.slice(0, 2),
        "nullable": bfloat16_array([1.0, None]),
        "vector": vectors,
        "tensors": tensors,
    })
    lance.write_dataset(table, path)
t = lance.dataset(path).to_table()
print(json.dumps({"schema": str(t.schema), "rows": t.to_pylist()}, default=str))
"""

IMAGE_SCRIPT = r"""
import json, os, pathlib, pickle, re, sys
import numpy as np, pyarrow as pa
from lance.arrow import ImageArray, ImageURIArray

d = sys.argv[1]
paths = [os.path.join(d, f"{i}.png") for i in range(3)]
for i, p in enumerate(paths):
    with open(p, "wb") as f:
        f.write(b"\\x89PNG\\r\\n\\x1a\\n" + bytes([i]) * 5)  # not decoded here: only the bytes are read
uris = ["file://" + p for p in paths]

def no_address(text):
    return re.sub(r"0x[0-9a-f]+", "0x", text)

uri_array = ImageURIArray.from_uris(uris)
out = {
    "uri_type": str(uri_array.type),
    "uri_repr": no_address(repr(uri_array)),
    "uri_list": uri_array.to_pylist() == uris,
    "from_large_string": ImageURIArray.from_uris(pa.array(uris, pa.large_string())).to_pylist() == uris,
    "from_paths": ImageURIArray.from_uris([pathlib.Path(p) for p in paths]).to_pylist() == [str(p) for p in paths],
    "from_array_is_same": ImageArray.from_array(uri_array) == uri_array,
    "uri_pickle": pickle.loads(pickle.dumps(uri_array)) == uri_array,
    "uri_scalar": [type(uri_array[0]).__name__, uri_array[0].as_py()],
}
encoded = uri_array.read_uris()
out["encoded_type"] = str(encoded.type)
out["encoded_list"] = [b.hex() for b in encoded.to_pylist()]
out["encoded_large"] = ImageArray.from_array(encoded.storage.cast(pa.large_binary())).to_pylist() == encoded.to_pylist()
out["encoded_scalar"] = type(encoded[0]).__name__
out["encoded_pickle"] = pickle.loads(pickle.dumps(encoded)) == encoded

tensor = pa.FixedShapeTensorArray.from_numpy_ndarray(np.arange(12, dtype=np.uint8).reshape(3, 1, 1, 4))
images = ImageArray.from_array(tensor)
out["tensor_type"] = str(images.type)
out["tensor_numpy"] = images.to_numpy().tolist()
out["tensor_pickle"] = pickle.loads(pickle.dumps(images)) == images
out["tensor_scalar"] = [type(images[0]).__name__, images[0].as_py()]
out["tensor_storage"] = str(images.storage.type)
re_encoded = images.to_encoded(encoder=lambda x: pa.array([x[i].tobytes() for i in range(len(x))], pa.binary()))
out["to_encoded"] = [b.hex() for b in re_encoded.to_pylist()]
decoded = re_encoded.to_tensor(decoder=lambda b: np.stack([np.frombuffer(v, np.uint8).reshape(1, 1, 4) for v in b.to_pylist()]))
out["to_tensor_custom"] = [str(decoded.type), decoded.to_numpy().tolist()]
print(json.dumps(out, default=str))
"""


def _json(engine: str, script: str, *args: str):
    return json.loads(_run(engine, script, *args).stdout)


def test_bf16_bit_patterns_and_reprs_match_pylance():
    require_pylance()
    pytest.importorskip("ml_dtypes")
    expected, actual = (_json(engine, BF16_EDGE_SCRIPT) for engine in ENGINES)
    assert len(actual["scalars"]) == len(expected["scalars"]) > 500
    mismatches = [(i, e, a) for i, (e, a) in enumerate(zip(expected["scalars"], actual["scalars"])) if e != a]
    assert mismatches == []
    assert actual["casts"] == expected["casts"]


def test_bf16_scalar_and_array_api_matches_pylance():
    require_pylance()
    pytest.importorskip("ml_dtypes")
    expected, actual = (_json(engine, BF16_API_SCRIPT) for engine in ENGINES)
    assert actual == expected


def test_bf16_pandas_and_numpy_match_pylance():
    require_pylance()
    pytest.importorskip("ml_dtypes")
    pytest.importorskip("pandas")
    expected, actual = (_json(engine, BF16_PANDAS_SCRIPT) for engine in ENGINES)
    assert actual == expected


def test_bf16_dataset_round_trip_reads_in_both_engines(tmp_path):
    require_pylance()
    pytest.importorskip("ml_dtypes")
    results = {}
    for writer in ENGINES:
        path = str(tmp_path / f"{writer}.lance")
        _run(writer, BF16_IO_SCRIPT, "write", path)
        for reader in ENGINES:
            results[(writer, reader)] = _json(reader, BF16_IO_SCRIPT, "read", path)
    reference = results[("pylance", "pylance")]
    assert "extension<lance.bfloat16<BFloat16Type>>" in reference["schema"]
    for key, value in results.items():
        assert value == reference, key


def test_image_types_match_pylance(tmp_path):
    require_pylance()
    # both engines read the same files, so the URIs they report are identical
    expected = _json("pylance", IMAGE_SCRIPT, str(tmp_path))
    actual = _json("nanolance", IMAGE_SCRIPT, str(tmp_path))
    assert actual == expected
    assert actual["encoded_scalar"] == "EncodedImageScalar"


# ---------------------------------------------------------------------------------------------
# lance.sampler and lance._dataset.sharded_batch_iterator (docs/tasks/PY_SURFACE.md, P3)

SAMPLER_DATA_SCRIPT = """
import sys
import pyarrow as pa
tbl = pa.table({"id": pa.array(range(2000), pa.int64()), "v": pa.array([float(i) for i in range(2000)]),
                "tag": pa.array([f"t{i % 5}" for i in range(2000)])})
ds = lance.write_dataset(tbl, sys.argv[1], max_rows_per_file=300, max_rows_per_group=64)
ds.delete("id % 7 = 3")
print(len(ds), len(ds.get_fragments()))
"""

SAMPLER_SCRIPT = """
import json, sys, warnings
from lance.sampler import (FullScanSampler, ShardedBatchSampler, ShardedFixedBatchSampler,
                           ShardedFragmentSampler, maybe_sample, reservoir_sampling)
from lance._dataset.sharded_batch_iterator import ShardedBatchIterator
warnings.simplefilter("ignore")
ds = lance.dataset(sys.argv[1])
cases = json.loads(sys.argv[2])


def rows(batches):
    return [[b.num_rows, b["id"].to_pylist()] for b in batches]


def run(c):
    kind, kw = c["kind"], c["kw"]
    if kind == "fragment":
        return rows(ShardedFragmentSampler(kw["rank"], kw["ws"], randomize=kw["randomize"], seed=kw["seed"])(
            ds, batch_size=kw["bs"], columns=["id"], filter=kw["filter"]))
    if kind == "full":
        return rows(FullScanSampler()(ds, batch_size=kw["bs"], columns=["id"], filter=kw["filter"]))
    if kind == "batch":
        return rows(ShardedBatchSampler(kw["rank"], kw["ws"], randomize=kw["randomize"], seed=kw["seed"])(
            ds, batch_size=kw["bs"], columns=["id"], filter=kw["filter"]))
    if kind == "fixed":
        sampler = ShardedFixedBatchSampler(kw["rank"], kw["ws"], randomize=kw["randomize"], seed=kw["seed"],
                                           batch_size=kw["bs"], total_num_rows=len(ds))
        return [list(b) for b in sampler]
    if kind == "maybe":
        import numpy as np
        np.random.seed(kw["seed"])
        return rows(maybe_sample(ds, kw["n"], ["id"], batch_size=kw["bs"], max_takes=kw["max_takes"],
                                 filt=kw["filter"]))
    if kind == "iter":
        return rows(ShardedBatchIterator(ds, kw["rank"], kw["ws"], columns=["id"], batch_size=kw["bs"],
                                         granularity=kw["granularity"]))
    if kind == "reservoir":
        got = reservoir_sampling(range(1000), 10)
        return [len(got), len(set(got)), all(0 <= x < 1000 for x in got)]
    raise ValueError(kind)


out = {}
for c in cases:
    try:
        out[c["name"]] = run(c)
    except Exception as exc:  # the same error on both engines is a match
        out[c["name"]] = {"error": type(exc).__name__}
print(json.dumps(out, default=str))
"""


@pytest.fixture(scope="module")
def sampler_dataset(tmp_path_factory):
    require_pylance()
    path = str(tmp_path_factory.mktemp("sampler") / "data.lance")
    _run("pylance", SAMPLER_DATA_SCRIPT, path)
    return path


def _sampler_cases():
    cases = []
    for ws in (1, 3, 4):
        for rank in range(ws):
            for randomize, seed in ((False, 0), (True, 5)):
                for filt in (None, "id < 1000"):
                    kw = dict(rank=rank, ws=ws, randomize=randomize, seed=seed, bs=100, filter=filt)
                    cases.append({"name": f"fragment-{rank}/{ws}-{randomize}-{seed}-{filt}", "kind": "fragment", "kw": kw})
                    cases.append({"name": f"batch-{rank}/{ws}-{randomize}-{seed}-{filt}-bs", "kind": "batch",
                                  "kw": dict(kw, bs=64 if rank else 1)})
                for bs in (7, 250):
                    cases.append({"name": f"fixed-{rank}/{ws}-{randomize}-{seed}-{bs}", "kind": "fixed",
                                  "kw": dict(rank=rank, ws=ws, randomize=randomize, seed=seed, bs=bs)})
    for filt in (None, "id % 2 = 0", "id is not null"):
        for bs in (100, 1024):
            cases.append({"name": f"batch-filter-{filt}-{bs}", "kind": "batch",
                          "kw": dict(rank=2, ws=4, randomize=True, seed=1, bs=bs, filter=filt)})
    cases.append({"name": "full", "kind": "full", "kw": dict(bs=100, filter=None)})
    cases.append({"name": "full-filter", "kind": "full", "kw": dict(bs=100, filter="id < 300")})
    for seed in (0, 1, 2):
        for n, max_takes, filt in ((10, 2048, None), (300, 32, None), (100, 8, "id is not null"),
                                   (5000, 2048, None), (1500, 64, None)):
            cases.append({"name": f"maybe-{seed}-{n}-{max_takes}-{filt}", "kind": "maybe",
                          "kw": dict(seed=seed, n=n, bs=64, max_takes=max_takes, filter=filt)})
    for gran in ("fragment", "batch"):
        for rank in (0, 1):
            cases.append({"name": f"iter-{gran}-{rank}", "kind": "iter",
                          "kw": dict(rank=rank, ws=2, bs=100, granularity=gran)})
    cases.append({"name": "reservoir", "kind": "reservoir", "kw": {}})
    return cases


def test_sampler_outputs_match_pylance(sampler_dataset):
    cases = _sampler_cases()
    args = json.dumps(cases)
    expected, actual = (_json(engine, SAMPLER_SCRIPT, sampler_dataset, args) for engine in ENGINES)
    assert len(actual) == len(cases)
    # not vacuous: the full scan returns every live row, and the sharded cases returned rows
    assert sum(b[0] for b in expected["full"]) == sum(1 for i in range(2000) if i % 7 != 3)
    assert all(expected[name] for name in ("fragment-0/4-False-0-None", "maybe-1-300-32-None", "iter-batch-0"))
    mismatches = sorted(name for name in expected if expected[name] != actual[name])
    assert mismatches == []


def test_sampler_shards_partition_the_dataset(sampler_dataset):
    require_pylance()
    cases = [{"name": "full", "kind": "full", "kw": dict(bs=100, filter=None)}]
    for ws in (1, 3, 4):
        for rank in range(ws):
            cases.append({"name": f"fragment-{rank}-{ws}", "kind": "fragment",
                          "kw": dict(rank=rank, ws=ws, randomize=True, seed=3, bs=50, filter=None)})
            cases.append({"name": f"batch-{rank}-{ws}", "kind": "batch",
                          "kw": dict(rank=rank, ws=ws, randomize=False, seed=0, bs=50, filter=None)})
    for engine in ENGINES:
        got = _json(engine, SAMPLER_SCRIPT, sampler_dataset, json.dumps(cases))
        logical = sorted(i for i in range(2000) if i % 7 != 3)
        assert sorted(sum((b[1] for b in got["full"]), [])) == logical, engine
        for ws in (1, 3, 4):
            for kind in ("fragment", "batch"):
                rows = sorted(i for rank in range(ws) for b in got[f"{kind}-{rank}-{ws}"] for i in b[1])
                assert rows == logical, (engine, kind, ws)
        reservoir = _json(engine, SAMPLER_SCRIPT, sampler_dataset,
                          json.dumps([{"name": "r", "kind": "reservoir", "kw": {}}]))["r"]
        assert reservoir == [10, 10, True], engine


# ---------------------------------------------------------------------------------------------
# lance.tokenize: the analyzer of an INVERTED index on a query string (tests/test_lance.py of pylance)
# ---------------------------------------------------------------------------------------------

TOKENIZE_SCRIPT = r"""
import json, sys
out = []
for query, options in json.loads(sys.argv[1]):
    try:
        tokens = lance.tokenize(query, **options)
    except Exception as exc:
        out.append({"error": [type(exc).__name__, str(exc)]})
    else:
        out.append({"tokens": [[t.text, t.position] for t in tokens], "repr": [repr(t) for t in tokens]})
print(json.dumps(out))
"""

# Each case: (query, options). Both engines must give the same tokens, positions and reprs.
TOKENIZE_CASES = [
    ("the cats and dogs", {}),
    ("The Cats AND Dogs are running quickly", {}),
    ("the skip alpha", {"stem": False, "custom_stop_words": ["skip"]}),
    ("the skip alpha", {"remove_stop_words": False}),
    ("the cats", {"custom_stop_words": ["cat"]}),
    ("The", {"custom_stop_words": ["THE"]}),
    ("running jumps", {"stem": True, "remove_stop_words": False}),
    ("running jumps", {"stem": False}),
    ("runs running", {"stem": True, "remove_stop_words": False, "max_token_length": 4}),
    ("running", {"language": "English", "stem": True}),
    ("the Cats", {"base_tokenizer": "raw", "remove_stop_words": True}),
    ("Hello-World foo.bar", {"base_tokenizer": "whitespace"}),
    ("Hello-World foo.bar", {"base_tokenizer": "raw"}),
    ("tab\tsep\nnewline", {"base_tokenizer": "raw", "lower_case": False}),
    ('say "hi" it', {"base_tokenizer": "raw"}),
    ("the way   the  cats", {"base_tokenizer": "whitespace", "custom_stop_words": ["the"]}),
    ("hello", {"base_tokenizer": "ngram"}),
    ("hello world", {"base_tokenizer": "ngram", "min_ngram_length": 2, "max_ngram_length": 3}),
    ("hello", {"base_tokenizer": "ngram", "min_ngram_length": 2, "max_ngram_length": 3, "prefix_only": True}),
    ("hello", {"base_tokenizer": "ngram", "min_ngram_length": 3, "max_ngram_length": 3, "lower_case": False}),
    ("getUserName", {"analyzer": "code", "split_identifiers": True}),
    ("getUserName", {"analyzer": "code"}),
    ("getUserName2Abc", {"analyzer": "code", "split_identifiers": True, "split_on_numerics": True,
                         "preserve_original": False}),
    ("camelCaseWord and snake_case_name", {"analyzer": "code", "split_identifiers": True}),
    ("a::b", {"analyzer": "code", "index_operators": True}),
    ("a::b", {"analyzer": "code"}),
    ("foo->bar.baz::qux", {"analyzer": "code", "index_operators": True}),
    ("the Cats!!", {"analyzer": "code", "stem": True, "remove_stop_words": True}),
    ("", {}),
    ("   ", {}),
    ("the", {}),
    ("the", {"remove_stop_words": False, "stem": False}),
    ("x" * 41, {}),
    ("x" * 40, {}),
    ("x" * 41, {"max_token_length": None}),
    ("x" * 41, {"max_token_length": 3}),
    ("the cats", {"max_token_length": 0}),
    ("café naïve Ünïcödé ß", {}),
    ("café naïve Ünïcödé ß", {"ascii_folding": False}),
    ("Straße ＡＢＣ fullwidth", {}),
    ("Ǆ İ ı", {}),
    ("日本語 テキスト 中文", {}),
    ("Привет мир", {}),
    ("Running Cats", {"lower_case": False}),
    ("emoji 😀 smile", {}),
    ("hello_world foo-bar", {}),
    ("it's don't", {}),
]

# Each case: (query, options, phrase) -- both engines must raise the same exception type, with the phrase in
# their messages (pylance adds a "Failed to build tokenizer" prefix and a source location).
TOKENIZE_ERRORS = [
    ("getUserName", {"split_identifiers": True}, "code analyzer flags require analyzer='code'"),
    ("x", {"analyzer": "text", "base_tokenizer": "code"}, "base_tokenizer='code' requires analyzer='code'"),
    ("x", {"analyzer": "code", "base_tokenizer": "simple"}, "analyzer='code' requires base_tokenizer='code'"),
    ("x", {"analyzer": "bogus"}, "unknown analyzer 'bogus', expected 'text' or 'code'"),
    ("hello", {"base_tokenizer": "unknown"}, "unknown base tokenizer unknown"),
    ("hello", {"base_tokenizer": "ngram", "min_ngram_length": 3, "max_ngram_length": 2},
     "min_gram must not be greater than max_gram"),
    ("hello", {"base_tokenizer": "ngram", "min_ngram_length": 0, "max_ngram_length": 2},
     "min_gram must be greater than 0"),
]

# Options nanolance's analyzer does not implement (pylance tokenizes them): tokenize raises what
# create_scalar_index raises, the same type and message.
TOKENIZE_UNSUPPORTED = [
    ("Bonjour le monde", {"language": "French"}),
    ("Привет мир", {"language": "Russian"}),
    ("the cats", {"language": "French", "remove_stop_words": True, "stem": False}),
    ("hello", {"base_tokenizer": "jieba/default"}),
    ("hello", {"base_tokenizer": "icu"}),
    ("hello", {"base_tokenizer": "lindera/ipadic"}),
    ("hello", {"base_tokenizer": "unknown"}),
    ("getUserName", {"split_identifiers": True}),
]

TOKENIZE_UNSUPPORTED_SCRIPT = r"""
import json, sys
import pyarrow as pa
path, cases = sys.argv[1], json.loads(sys.argv[2])
lance.write_dataset(pa.table({"text": ["x"]}), path)
ds = lance.dataset(path)

def raised(call):
    try:
        call()
    except Exception as exc:
        return [type(exc).__name__, str(exc)]
    return None

out = []
for query, options in cases:
    out.append([raised(lambda: lance.tokenize(query, **options)),
                raised(lambda: ds.create_scalar_index("text", index_type="INVERTED", **options))])
print(json.dumps(out))
"""


def test_tokenize_matches_pylance(tmp_path):
    require_pylance()
    cases = TOKENIZE_CASES + [(query, options) for query, options, _ in TOKENIZE_ERRORS]
    expected, actual = (_json(engine, TOKENIZE_SCRIPT, json.dumps(cases)) for engine in ENGINES)
    assert len(actual) == len(expected) == len(cases)
    ok = len(TOKENIZE_CASES)
    mismatches = [(i, e, a) for i, (e, a) in enumerate(zip(expected[:ok], actual[:ok])) if e != a]
    assert mismatches == []
    for (query, options, phrase), e, a in zip(TOKENIZE_ERRORS, expected[ok:], actual[ok:]):
        assert "error" in e and "error" in a, (query, options)
        assert e["error"][0] == a["error"][0], (query, options, e, a)
        assert phrase in e["error"][1] and phrase in a["error"][1], (query, options, e, a)


def test_tokenize_errors_match_create_scalar_index(tmp_path):
    results = _json("nanolance", TOKENIZE_UNSUPPORTED_SCRIPT, str(tmp_path / "t.lance"),
                    json.dumps(TOKENIZE_UNSUPPORTED))
    assert len(results) == len(TOKENIZE_UNSUPPORTED)
    for (query, options), (tokenize_error, index_error) in zip(TOKENIZE_UNSUPPORTED, results):
        assert tokenize_error is not None, (query, options)
        assert tokenize_error == index_error, (query, options)


def test_tokenize_result_is_fts_tokens_with_pylance_repr():
    require_pylance()
    expected = _json("pylance", TOKENIZE_SCRIPT, json.dumps([["the cats", {}]]))
    actual = _json("nanolance", TOKENIZE_SCRIPT, json.dumps([["the cats", {}]]))
    assert actual == expected == [{"tokens": [["cat", 0]], "repr": ['FtsToken(text="cat", position=0)']}]


# ---------------------------------------------------------------------------------------------
# lance.Session: budgets, sizes, sharing and errors (tests/test_session.py of pylance)
# nanolance's caches are process-wide, so its sizes are per-session estimates (docs/tasks/PY_SURFACE_2.md)
# ---------------------------------------------------------------------------------------------
SESSION_ERROR_SCRIPT = r"""
import json, re, sys
out = []
for kwargs in json.loads(sys.argv[1]):
    try:
        session = lance.Session(**kwargs)
    except Exception as exc:
        out.append([type(exc).__name__, re.sub(r", /home/runner/\S+$", "", str(exc))])
    else:
        out.append(["ok", repr(session), session.index_cache_size_bytes(), session.size_bytes()])
print(json.dumps(out))
"""

# Each case: the keyword arguments of lance.Session. Both engines must raise the same exception type with the
# same message (pylance's Rust source location aside), or return the same repr and sizes.
SESSION_CASES = [
    {"index_cache_size_bytes": 1024, "index_cache_backend": "moka://?capacity=1048576"},
    {"metadata_cache_size_bytes": 1, "metadata_cache_backend": "moka://?capacity=5"},
    {"index_cache_backend": "moka://"},
    {"index_cache_backend": "moka://?capacity="},
    {"index_cache_backend": "moka://?capacity=abc"},
    {"index_cache_backend": "moka://?capacity=-1"},
    {"index_cache_backend": "foo://?capacity=1"},
    {"index_cache_backend": "1048576"},
    {"index_cache_backend": ""},
    {"index_cache_backend": {"kind": "moka", "capacity": "1048576"}},
    {"index_cache_backend": {"options": {"capacity": "1"}}},
    {"index_cache_backend": {"kind": 5, "options": {}}},
    {"index_cache_backend": {"kind": "redis", "options": {}}},
    {"index_cache_backend": {"kind": "moka"}},
    {"index_cache_backend": {"kind": "moka", "options": {"capacity": 5}}},
    {"index_cache_backend": {"kind": "moka", "options": "capacity=1"}},
    {"index_cache_backend": 5},
    {"metadata_cache_backend": "moka://"},
    {"index_cache_size_bytes": -1},
    {"index_cache_size_bytes": 1.5},
    {"index_cache_backend": "moka://?capacity=1048576"},
    {"index_cache_backend": {"kind": "MOKA", "options": {"capacity": "1048576"}}},
    {"index_cache_size_bytes": 0, "metadata_cache_size_bytes": 0},
    {},
]


def test_session_errors_and_reprs_match_pylance():
    require_pylance()
    payload = json.dumps(SESSION_CASES)
    expected, actual = (_json(engine, SESSION_ERROR_SCRIPT, payload) for engine in ENGINES)
    assert len(actual) == len(expected) == len(SESSION_CASES)
    for case, e, a in zip(SESSION_CASES, expected, actual):
        assert e == a, (case, e, a)
    # The accepted forms are fresh sessions: nothing recorded yet, so the reprs read 0 and the size is the overhead.
    accepted = [a for a in actual if a[0] == "ok"]
    assert len(accepted) == 4
    assert all(a[1] == "Session(index_cache_size_bytes=0, metadata_cache_size_bytes=0)" and a[3] == 152
               for a in accepted)


SESSION_STATE_SCRIPT = r"""
import json, sys
import pyarrow as pa
from lance.fragment import write_fragments
path = sys.argv[1]
lance.write_dataset(pa.table({"a": range(1000)}), path, max_rows_per_file=250)
out = {}
ds = lance.dataset(path)
s = ds.session()
opened = s.size_bytes()
ds.scanner().to_table()
out["grows_after_scan"] = s.size_bytes() > opened > 0 or s.size_bytes() > opened
out["repr_is_session"] = repr(s).startswith("Session(index_cache_size_bytes=0, metadata_cache_size_bytes=")
zero = lance.dataset(path, metadata_cache_size_bytes=0)
zero.to_table()
out["zero_budget_size"] = zero.session().size_bytes()
out["zero_budget_smaller"] = zero.session().size_bytes() < s.size_bytes()
out["shared_is_same"] = lance.dataset(path, session=s).session().is_same_as(s)
out["fresh_sessions_differ"] = not lance.dataset(path).session().is_same_as(lance.dataset(path).session())
out["is_same_as_new_session"] = s.is_same_as(lance.Session())
try:
    s.is_same_as(None)
except TypeError:
    out["is_same_as_rejects_none"] = True
before = s.size_bytes()
shared = lance.dataset(path, session=s)
shared.to_table()
out["shared_read_grows_once"] = s.size_bytes() >= before
out["index_cache_zero_without_index"] = s.index_cache_size_bytes() == 0
dest = lance.dataset(path)
owner = dest.session()
before = owner.size_bytes()
write_fragments(pa.table({"a": [1]}), path, mode="append", session=owner)
out["write_fragments_grows"] = owner.size_bytes() > before
try:
    write_fragments(pa.table({"a": [1]}), dest, mode="append", session=lance.Session())
except ValueError as exc:
    out["foreign_session_message"] = "not the destination dataset's own session" in str(exc)
print(json.dumps(out, sort_keys=True))
"""


def test_session_state_matches_pylance(tmp_path):
    require_pylance()
    expected, actual = (_json(engine, SESSION_STATE_SCRIPT, str(tmp_path / engine)) for engine in ENGINES)
    assert actual == expected
    assert expected["zero_budget_size"] == 152
    assert expected["grows_after_scan"] and expected["write_fragments_grows"]


SESSION_INDEX_SCRIPT = r"""
import json, os, sys
import numpy as np, pyarrow as pa
path = sys.argv[1]
vec = np.random.default_rng(0).standard_normal((200, 8)).astype("float32")
table = pa.table({"id": pa.array(range(200)),
                  "v": pa.FixedSizeListArray.from_arrays(pa.array(vec.ravel()), 8)})
lance.write_dataset(table, path, max_rows_per_file=100).create_index("v", "IVF_FLAT", num_partitions=2,
                                                                     metric="l2")
query = {"column": "v", "q": vec[3].tolist(), "k": 3}
out = {}
ds = lance.dataset(path)
s = ds.session()
before = s.index_cache_size_bytes()
out["ids"] = ds.to_table(nearest=query, columns=["id"]).column("id").to_pylist()
after = s.index_cache_size_bytes()
out["index_grows_on_search"] = after > before
index_uuid = ds.list_indices()[0]["uuid"]
index_dir = os.path.join(path, "_indices", str(index_uuid))
out["index_is_on_disk_size"] = after == sum(os.path.getsize(os.path.join(root, f))
                                            for root, _, files in os.walk(index_dir) for f in files)
out["size_includes_index"] = s.size_bytes() == 152 + s.metadata_cache_size_bytes() + after
no_index = lance.dataset(path, index_cache_size_bytes=0)
out["zero_index_budget_ids"] = no_index.to_table(nearest=query, columns=["id"]).column("id").to_pylist()
out["zero_index_budget_size"] = no_index.session().index_cache_size_bytes()
shared = lance.dataset(path, session=s)
shared.to_table(nearest=query, columns=["id"])
out["shared_search_counts_once"] = s.index_cache_size_bytes() == after
small = lance.Session(index_cache_size_bytes=1)
lance.dataset(path, session=small).to_table(nearest=query, columns=["id"])
out["small_budget_refuses_index"] = (small.index_cache_size_bytes() == 0
                                     and small.size_bytes() == 152 + small.metadata_cache_size_bytes())
print(json.dumps(out, sort_keys=True))
"""


def test_session_index_entries_are_on_disk_sizes(tmp_path):
    out = _json("nanolance", SESSION_INDEX_SCRIPT, str(tmp_path / "vectors.lance"))
    assert out["ids"] == out["zero_index_budget_ids"]
    assert out["ids"][0] == 3
    for key in ("index_grows_on_search", "index_is_on_disk_size", "size_includes_index",
                "shared_search_counts_once", "small_budget_refuses_index"):
        assert out[key] is True, key
    assert out["zero_index_budget_size"] == 0


# ---------------------------------------------------------------------------------------------
# batch KNN: nearest={"q": 2-D array} (tests/test_vector_index.py of pylance)
# ---------------------------------------------------------------------------------------------

BATCH_SCRIPT = """
import json, os, sys, warnings
import numpy as np, pyarrow as pa
warnings.filterwarnings("ignore")
base, kind = sys.argv[1], sys.argv[2]
dim, n = 16, 300
rng = np.random.default_rng(7)

def table(lo, hi):
    ids = np.arange(lo, hi)
    vec = rng.standard_normal((hi - lo, dim)).astype(np.float32)
    return pa.table({"id": pa.array(ids, pa.int64()), "tag": pa.array([f"t{i % 3}" for i in ids]),
                     "vector": pa.FixedSizeListArray.from_arrays(pa.array(vec.ravel()), dim)})

path = os.path.join(base, kind + ".lance")
ds = lance.write_dataset(table(0, n), path, max_rows_per_file=100)
if kind != "flat":
    ds.create_index("vector", index_type=kind, num_partitions=4, **({"num_sub_vectors": 4} if kind == "IVF_PQ" else {}))
    ds = lance.write_dataset(table(n, n + 40), path, mode="append")  # rows the index does not cover
ds.delete("id % 7 = 0")
ds = lance.dataset(path)
queries = rng.standard_normal((3, dim)).astype(np.float32)
flat = kind == "flat"
base_nn = {"k": 5} if flat else {"k": 5, "nprobes": 4}
configs = {
    "plain": (dict(base_nn), {}, ("id",), queries),
    "filter_prefilter": (dict(base_nn, k=4), {"filter": "tag = 't1'", "prefilter": True}, ("id",), queries),
    "filter_postfilter": (dict(base_nn, k=4), {"filter": "tag = 't1'"}, ("id",), queries),
    "distance_range": (dict(base_nn, distance_range=(1.0, 4.0)), {}, ("id",), queries),
    "limit": (dict(base_nn), {"limit": 7}, ("id",), queries),
    "offset": (dict(base_nn), {"offset": 4, "limit": 6}, ("id",), queries),
    "columns_row_id": (dict(base_nn, k=3), {}, ("id", "_rowid", "tag"), queries),
    "with_row_id": (dict(base_nn, k=3), {"with_row_id": True}, ("id", "tag"), queries),
    "single_row": (dict(base_nn, k=3), {}, ("id",), queries[:1]),
    "list_of_lists": (dict(base_nn, k=3), {}, ("id",), queries.tolist()),
    "fast_search": (dict(base_nn, k=3), {"fast_search": True}, ("id",), queries),
}
if not flat:
    configs["refine_factor"] = (dict(base_nn, refine_factor=2), {}, ("id",), queries)

def run(nn, kw, cols, q):
    extra = {k: v for k, v in kw.items() if k not in ("limit", "offset")}
    def call(qq, opts):
        return ds.to_table(columns=list(cols), nearest=dict(nn, column="vector", q=qq), **opts)
    batch = call(q, kw)
    parts = []
    for i, row in enumerate(np.asarray(q, np.float32)):
        parts += [dict(r, query_index=i) for r in call(row, extra).to_pylist()]
    off, lim = kw.get("offset", 0), kw.get("limit")
    expected = parts[off:] if lim is None else parts[off:off + lim]
    got = batch.to_pylist()
    has_qi = "query_index" in batch.column_names
    return {"names": batch.column_names, "query_index": batch["query_index"].to_pylist() if has_qi else None,
            "ids": batch["id"].to_pylist(), "distances": [round(x, 4) for x in batch["_distance"].to_pylist()],
            "matches_singles": got == expected, "rows": len(got)}

print(json.dumps({name: run(*spec) for name, spec in configs.items()}, sort_keys=True))
"""

ERRORS_SCRIPT = """
import json, sys, warnings
import numpy as np, pyarrow as pa
warnings.filterwarnings("ignore")
base = sys.argv[1]
dim, n = 16, 60
rng = np.random.default_rng(3)
vec = rng.standard_normal((n, dim)).astype(np.float32)
arr = pa.FixedSizeListArray.from_arrays(pa.array(vec.ravel()), dim)
ds = lance.write_dataset(pa.table({"id": pa.array(range(n), pa.int64()), "vector": arr}), base + "/e.lance")
qi = lance.write_dataset(pa.table({"id": pa.array(range(n), pa.int64()),
                                   "query_index": pa.array(range(n), pa.uint32()), "vector": arr}), base + "/q.lance")
two = rng.standard_normal((2, dim)).astype(np.float32)

def attempt(target, **nn):
    try:
        target.to_table(columns=["id"], nearest=dict({"column": "vector", "k": 3, "use_index": False}, **nn))
        return "ok"
    except Exception as e:
        return [type(e).__name__, str(e).split(", /")[0]]

cases = {
    "dim_1d": attempt(ds, q=np.zeros(256, np.float32)),
    "dim_2d": attempt(ds, q=np.zeros((2, 20), np.float32)),
    "dim_one_row": attempt(ds, q=np.zeros((1, 20), np.float32)),
    "k_zero_batch": attempt(ds, q=two, k=0),
    "arrow_fixed_size_list": attempt(ds, q=pa.array(two.tolist(), pa.list_(pa.float32(), dim))),
    "arrow_list": attempt(ds, q=pa.array(two.tolist(), pa.list_(pa.float32()))),
    "query_index_column_batch": attempt(qi, q=two),
    "query_index_column_single": attempt(qi, q=two[0]),
}
print(json.dumps(cases, sort_keys=True))
"""


@pytest.mark.parametrize("kind", ["flat", "IVF_FLAT", "IVF_PQ", "IVF_HNSW_SQ"])
def test_batch_knn_matches_pylance_and_single_queries(tmp_path, kind):
    require_pylance()
    out = {}
    for engine in ENGINES:
        (tmp_path / engine).mkdir()
        out[engine] = _json(engine, BATCH_SCRIPT, str(tmp_path / engine), kind)
    expected, actual = out["pylance"], out["nanolance"]
    assert actual.keys() == expected.keys()
    for name, got in actual.items():
        assert got["matches_singles"] is True, name
        if kind != "flat" and name == "filter_postfilter":
            # a post-filter keeps the rows of each query's approximate top k that pass it: how many depends
            # on the engine's own index, so only the shape of query_index is shared (queries in order, at
            # most k rows each)
            qi = got["query_index"]
            assert qi == sorted(qi) and all(q >= 0 and qi.count(q) <= 4 for q in qi), name
            continue
        assert got["query_index"] == expected[name]["query_index"], name
        if name == "fast_search":
            # the scan skips unindexed rows: none without an index, the indexed ones with one (pylance also
            # lists _rowid here, nanolance's single does not)
            if kind == "flat":
                assert got["ids"] == [] and "query_index" in got["names"], name
            else:
                assert len(got["ids"]) == len(expected[name]["ids"]), name
            continue
        assert got["names"] == expected[name]["names"], name
        if kind == "flat":
            # exact search: the same rows and distances as pylance
            assert got["ids"] == expected[name]["ids"], name
            assert got["distances"] == expected[name]["distances"], name
        else:
            # an index is built by each engine: the same shape, not the same approximate rows
            assert len(got["ids"]) == len(expected[name]["ids"]), name


def test_batch_errors_match_pylance(tmp_path):
    require_pylance()
    out = {}
    for engine in ENGINES:
        (tmp_path / engine).mkdir()
        out[engine] = _json(engine, ERRORS_SCRIPT, str(tmp_path / engine))
    expected, actual = out["pylance"], out["nanolance"]
    assert actual == expected
    assert expected["query_index_column_single"] == "ok"
    assert expected["dim_1d"] == ["ValueError", "Query vector size 256 does not match index column size 16"]
