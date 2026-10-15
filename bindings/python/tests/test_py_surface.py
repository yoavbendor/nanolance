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
